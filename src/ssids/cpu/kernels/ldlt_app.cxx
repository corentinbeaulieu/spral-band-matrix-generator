/* Copyright 2016 The Science and Technology Facilities Council (STFC)
 *
 * Authors: Jonathan Hogg (STFC)
 *
 * IMPORTANT: This file is NOT licenced under the BSD licence. If you wish to
 * licence this code, please contact STFC via hsl@stfc.ac.uk
 * (We are currently deciding what licence to release this code under if it
 * proves to be useful beyond our own academic experiments)
 *
 */
#include "ldlt_app.hxx"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <ostream>
#include <sstream>
#include <utility>
#include <vector>

#include <omp.h>

#include "../AlignedAllocator.hxx"
#include "../BlockPool.hxx"
#include "block_ldlt.hxx"
#include "ldlt_tpp.hxx"
#include "common.hxx"
#include "wrappers.hxx"

namespace spral { namespace ssids { namespace cpu {

namespace ldlt_app {

bool is_aligned(void* ptr) {
   const int align = 32;
   return (reinterpret_cast<uintptr_t>(ptr) % align == 0);
}

template<typename T,
         bool debug=false,
         typename Alloc=AlignedAllocator<T>
         >
class CpuLDLT {
   static const int BLOCK_SIZE = 32;
private:
   /// u specifies the pivot tolerance
   const T u;
   /// small specifies the threshold for entries to be considered zero
   const T small;
   /// Allocator
   mutable Alloc alloc;

   /** Workspace allocated on a per-thread basis */
   struct ThreadWork {
      T *ld;

      ThreadWork() {
         ld = new T[BLOCK_SIZE*BLOCK_SIZE];
      }
      ~ThreadWork() {
         delete[] ld;
      }
   };

   struct col_data {
      int npad; //< Number of entries added for padding to start
      int nelim;
      int npass;
      omp_lock_t lock;
      int *perm;
      T *d;

      col_data() : 
         npad(0), nelim(0)
      {
         omp_init_lock(&lock);
      }
      ~col_data() {
         omp_destroy_lock(&lock);
      }

      /** Moves d and perm for eliminated columns to elim_d and elim_perm
       * (which may overlap with d and perm!). Puts uneliminated variables in
       * failed_perm (no need for d with failed vars). */
      void move_back(int* elim_perm, int* failed_perm) {
         if(perm+2*npad != elim_perm) { // Don't move if memory is identical
            for(int i=npad; i<nelim; ++i)
               *(elim_perm++) = perm[i];
         }
         // Copy failed perm
         for(int i=nelim; i<BLOCK_SIZE; ++i)
            *(failed_perm++) = perm[i];
      }

   };

   class BlockData {
      public:
      /// Latest accepted value of A or L
      alignas(32) T* aval;
      /// Trial value of L
      T *lwork;

      BlockData()
      : lwork(nullptr)
      {}

      void create_restore_point(int pad, int lda) {
         for(int j=pad; j<BLOCK_SIZE; j++)
         for(int i=pad; i<BLOCK_SIZE; i++)
            lwork[j*BLOCK_SIZE+i] = aval[j*lda+i];
      }

      /** Apply row permutation to block at same time as taking a copy */
      void create_restore_point_with_row_perm(int rpad, int cpad, const int *lperm, int lda) {
         for(int j=cpad; j<BLOCK_SIZE; j++)
         for(int i=rpad; i<BLOCK_SIZE; i++) {
            int r = lperm[i];
            lwork[j*BLOCK_SIZE+i] = aval[j*lda+r];
         }
         for(int j=cpad; j<BLOCK_SIZE; j++)
         for(int i=rpad; i<BLOCK_SIZE; i++)
            aval[j*lda+i] = lwork[j*BLOCK_SIZE+i];
      }

      /** Apply column permutation to block at same time as taking a copy */
      void create_restore_point_with_col_perm(int rpad, int cpad, const int *lperm, int lda) {
         for(int j=cpad; j<BLOCK_SIZE; j++) {
            int c = lperm[j];
            for(int i=rpad; i<BLOCK_SIZE; i++)
               lwork[j*BLOCK_SIZE+i] = aval[c*lda+i];
         }
         for(int j=cpad; j<BLOCK_SIZE; j++)
         for(int i=rpad; i<BLOCK_SIZE; i++)
            aval[j*lda+i] = lwork[j*BLOCK_SIZE+i];
      }

      /** Restores any columns that have failed back to their previous
       *  values stored in lwork[] */
      void restore_part(int rfrom, int cfrom, int lda) {
         for(int j=cfrom; j<BLOCK_SIZE; j++)
         for(int i=rfrom; i<BLOCK_SIZE; i++)
            aval[j*lda+i] = lwork[j*BLOCK_SIZE+i];
      }

      /** Restores any columns that have failed back to their previous
       *  values stored in lwork[]. Applies a symmetric permutation while
       *  doing so. */
      void restore_part_with_sym_perm(int from, const int *lperm, int lda) {
         for(int j=from; j<BLOCK_SIZE; j++) {
            int c = lperm[j];
            for(int i=from; i<BLOCK_SIZE; i++) {
               int r = lperm[i];
               aval[j*lda+i] = (r>c) ? lwork[c*BLOCK_SIZE+r]
                                      : lwork[r*BLOCK_SIZE+c];
            }
         }
      }

      /** Move up eliminated entries to fill any gaps left by failed pivots
       *  within diagonal block.
       *  Note that out and aval may overlap. */
      void move_up_diag(struct col_data const& idata, struct col_data const& jdata, T* out, int ldout, int lda) const {
         for(int j=jdata.npad, jout=0; j<jdata.nelim; ++j, ++jout)
         for(int i=idata.npad, iout=0; i<idata.nelim; ++i, ++iout)
            out[jout*ldout+iout] = aval[j*lda+i];
      }

      /** Move up eliminated entries to fill any gaps left by failed pivots
       *  within rectangular block of matrix.
       *  Note that out and aval may overlap. */
      void move_up_rect(int rfrom, struct col_data const& jdata, T* out, int ldout, int lda) const {
         for(int j=jdata.npad, jout=0; j<jdata.nelim; ++j, ++jout)
         for(int i=rfrom, iout=0; i<BLOCK_SIZE; ++i, ++iout)
            out[jout*ldout+iout] = aval[j*lda+i];
      }

      /** Copies failed rows and columns^T to specified locations */
      void copy_failed_diag(struct col_data const& idata, struct col_data const& jdata, T* rout, T* cout, T* dout, int ldout, int lda) const {
         /* copy rows */
         for(int j=jdata.npad, jout=0; j<jdata.nelim; ++j, ++jout)
         for(int i=idata.nelim, iout=0; i<BLOCK_SIZE; ++i, ++iout)
            rout[jout*ldout+iout] = aval[j*lda+i];
         /* copy cols in transpose (not for diagonal block) */
         if(&idata != &jdata) {
            for(int j=jdata.nelim, iout=0; j<BLOCK_SIZE; ++j, ++iout)
            for(int i=idata.npad, jout=0; i<idata.nelim; ++i, ++jout)
               cout[jout*ldout+iout] = aval[j*lda+i];
         }
         /* copy intersection of failed rows and cols */
         for(int j=jdata.nelim, jout=0; j<BLOCK_SIZE; j++, ++jout)
         for(int i=idata.nelim, iout=0; i<BLOCK_SIZE; ++i, ++iout)
            dout[jout*ldout+iout] = aval[j*lda+i];
      }

      /** Copies failed columns to specified location */
      void copy_failed_rect(int nrow, struct col_data const& jdata, T* cout, int ldout, int lda) const {
         for(int j=jdata.nelim, jout=0; j<BLOCK_SIZE; ++j, ++jout)
            for(int i=BLOCK_SIZE-nrow, iout=0; i<BLOCK_SIZE; ++i, ++iout)
               cout[jout*ldout+iout] = aval[j*lda+i];
      }

      /** Check if a block satisifies pivot threshold (colwise version) */
      template <enum operation op>
      int check_threshold(int rfrom, int cfrom, T u, int lda) {
         // Perform thrshold test for each uneliminated row/column
         for(int j=cfrom; j<BLOCK_SIZE; j++)
         for(int i=rfrom; i<BLOCK_SIZE; i++)
            if(fabs(aval[j*lda+i]) > 1.0/u) {
               if(debug) printf("Failed %d,%d:%e\n", i, j, fabs(aval[j*lda+i]));
               return (op==OP_N) ? j : i;
            }
         // If we get this far, everything is good
         return BLOCK_SIZE;
      }

      /** Performs solve with diagonal block \f$L_{21} = A_{21} L_{11}^{-T} D_1^{-1}\f$. Designed for below diagonal. */
      /* NB: d stores (inverted) pivots as follows:
       * 2x2 ( a b ) stored as d = [ a b Inf c ]
       *     ( b c )
       * 1x1  ( a )  stored as d = [ a 0.0 ]
       * 1x1  ( 0 ) stored as d = [ 0.0 0.0 ]
       */
      template <enum operation op>
      void apply_pivot(int rfrom, int cfrom, const T *diag, int ldd, const T *d, const T small, int lda) {
         if(rfrom >= BLOCK_SIZE || cfrom >= BLOCK_SIZE) return; // no-op

         if(op==OP_N) {
            // Perform solve L_11^-T
            host_trsm<T>(SIDE_RIGHT, FILL_MODE_LWR, OP_T, DIAG_UNIT, BLOCK_SIZE-rfrom, BLOCK_SIZE-cfrom, 1.0, &diag[cfrom*ldd+cfrom], ldd, &aval[cfrom*lda+rfrom], lda);
            // Perform solve L_21 D^-1
            for(int i=cfrom; i<BLOCK_SIZE; ) {
               if(i+1==BLOCK_SIZE || std::isfinite(d[2*i+2])) {
                  // 1x1 pivot
                  T d11 = d[2*i];
                  if(d11 == 0.0) {
                     // Handle zero pivots carefully
                     for(int j=rfrom; j<BLOCK_SIZE; j++) {
                        T v = aval[i*lda+j];
                        aval[i*lda+j] = 
                           (fabs(v)<small) ? 0.0
                                           : std::numeric_limits<T>::infinity()*v;
                        // NB: *v above handles NaNs correctly
                     }
                  } else {
                     // Non-zero pivot, apply in normal fashion
                     for(int j=rfrom; j<BLOCK_SIZE; j++)
                        aval[i*lda+j] *= d11;
                  }
                  i++;
               } else {
                  // 2x2 pivot
                  T d11 = d[2*i];
                  T d21 = d[2*i+1];
                  T d22 = d[2*i+3];
                  for(int j=rfrom; j<BLOCK_SIZE; j++) {
                     T a1 = aval[i*lda+j];
                     T a2 = aval[(i+1)*lda+j];
                     aval[i*lda+j]     = d11*a1 + d21*a2;
                     aval[(i+1)*lda+j] = d21*a1 + d22*a2;
                  }
                  i += 2;
               }
            }
         } else { /* op==OP_T */
            // Perform solve L_11^-1
            host_trsm<T>(SIDE_LEFT, FILL_MODE_LWR, OP_N, DIAG_UNIT, BLOCK_SIZE-rfrom, BLOCK_SIZE-cfrom, 1.0, &diag[rfrom*ldd+rfrom], ldd, &aval[cfrom*lda+rfrom], lda);
            // Perform solve D^-T L_21^T
            for(int i=rfrom; i<BLOCK_SIZE; ) {
               if(i+1==BLOCK_SIZE || std::isfinite(d[2*i+2])) {
                  // 1x1 pivot
                  T d11 = d[2*i];
                  if(d11 == 0.0) {
                     // Handle zero pivots carefully
                     for(int j=cfrom; j<BLOCK_SIZE; j++) {
                        T v = aval[j*lda+i];
                        aval[j*lda+i] = 
                           (fabs(v)<small) ? 0.0 // *v handles NaNs
                                           : std::numeric_limits<T>::infinity()*v;
                        // NB: *v above handles NaNs correctly
                     }
                  } else {
                     // Non-zero pivot, apply in normal fashion
                     for(int j=cfrom; j<BLOCK_SIZE; j++) {
                        aval[j*lda+i] *= d11;
                     }
                  }
                  i++;
               } else {
                  // 2x2 pivot
                  T d11 = d[2*i];
                  T d21 = d[2*i+1];
                  T d22 = d[2*i+3];
                  for(int j=cfrom; j<BLOCK_SIZE; j++) {
                     T a1 = aval[j*lda+i];
                     T a2 = aval[j*lda+(i+1)];
                     aval[j*lda+i]     = d11*a1 + d21*a2;
                     aval[j*lda+(i+1)] = d21*a1 + d22*a2;
                  }
                  i += 2;
               }
            }
         }
      }

      /** Apply successful pivot update to all uneliminated columns 
       *  (this.aval in non-transpose) */
      template<enum operation op>
      void update(int npad, int nelim, const T *l, int ldl, const T *ld, int ldld, int rfrom, int cfrom, int lda) {
         host_gemm(OP_N, (op==OP_N) ? OP_T : OP_N,
               BLOCK_SIZE-rfrom, BLOCK_SIZE-cfrom, nelim-npad,
               -1.0, &ld[npad*ldld+rfrom], ldld,
               (op==OP_N) ? &l[npad*ldl+cfrom] : &l[cfrom*ldl+npad], ldl,
               1.0, &aval[cfrom*lda+rfrom], lda);
      }

      // FIXME: debug only remove
      void print(int rpad, int cpad, int lda) const {
         for(int i=rpad; i<BLOCK_SIZE; ++i) {
            printf("%d:", i);
            for(int j=cpad; j<BLOCK_SIZE; ++j)
               printf(" %e", aval[j*lda+i]);
            printf("\n");
         }
      }

      // FIXME: debug only remove
      void check_nan_diag(int pad, int lda) const {
         for(int j=pad; j<BLOCK_SIZE; ++j)
            for(int i=j; i<BLOCK_SIZE; ++i)
               if(std::isnan(aval[j*lda+i])) {
                  printf("NaN at %d %d\n", i, j);
                  exit(1);
               }
      }

      // FIXME: debug only remove
      void check_nan(int rpad, int cpad, int lda) const {
         for(int j=cpad; j<BLOCK_SIZE; ++j)
            for(int i=rpad; i<BLOCK_SIZE; ++i)
               if(std::isnan(aval[j*lda+i])) {
                  printf("NaN at %d %d\n", i, j);
                  exit(1);
               }
      }
   };

   /** Calculates LD from L and D */
   template <enum operation op>
   static
   void calcLD(int m, int n, const T *l, int ldl, const T *d, T *ld, int ldld) {
      for(int col=0; col<n; ) {
         if(col+1==n || std::isfinite(d[2*col+2])) {
            // 1x1 pivot
            T d11 = d[2*col];
            if(d11 != 0.0) d11 = 1/d11; // Zero pivots just cause zeroes
            for(int row=0; row<m; row++)
               ld[col*ldld+row] = d11 * ((op==OP_N) ? l[col*ldl+row]
                                                    : l[row*ldl+col]);
            col++;
         } else {
            // 2x2 pivot
            T d11 = d[2*col];
            T d21 = d[2*col+1];
            T d22 = d[2*col+3];
            T det = d11*d22 - d21*d21;
            d11 = d11/det;
            d21 = d21/det;
            d22 = d22/det;
            for(int row=0; row<m; row++) {
               T a1 = (op==OP_N) ? l[col*ldl+row]     : l[row*ldl+col];
               T a2 = (op==OP_N) ? l[(col+1)*ldl+row] : l[row*ldl+(col+1)];
               ld[col*ldld+row]     =  d22*a1 - d21*a2;
               ld[(col+1)*ldld+row] = -d21*a1 + d11*a2;
            }
            col += 2;
         }
      }
   }


   bool run_elim(int &next_elim, int const m, int const n, const int mblk, const int nblk, struct col_data *cdata, BlockData *blkdata, T* d, int lda, BlockPool<T, BLOCK_SIZE> &global_work, ThreadWork all_thread_work[]) {
      bool changed = false;
      //printf("ENTRY %d %d vis %d %d %d\n", m, n, mblk, nblk, BLOCK_SIZE);

      // FIXME: is global_lperm really the best way?
      int *global_lperm = new int[nblk*BLOCK_SIZE];

      /* Inner loop - iterate over block columns */
      for(int blk=0; blk<nblk; blk++) {
         // Don't bother adding tasks if we eliminated everything already
         if(cdata[blk].npad>=BLOCK_SIZE) continue;

         if(debug) {
            printf("Bcol %d:\n", blk);
            print_mat(mblk, nblk, m, n, blkdata, cdata, lda);
         }

         // Factor diagonal: depend on cdata[blk] as we do some init here
         #pragma omp task default(none) \
            firstprivate(blk) \
            shared(blkdata, cdata, lda, global_lperm, global_work, \
                   all_thread_work, next_elim, d) \
            depend(inout: blkdata[blk*mblk+blk:1]) \
            depend(inout: cdata[blk:1])
         {
            //printf("Factor(%d)\n", blk);
            int thread_num = omp_get_thread_num();
            ThreadWork &thread_work = all_thread_work[thread_num];
            BlockData &dblk = blkdata[blk*mblk+blk];
            dblk.lwork = global_work.get_wait();
            int *lperm = &global_lperm[blk*BLOCK_SIZE];
            for(int i=0; i<BLOCK_SIZE; i++)
               lperm[i] = i;
            int dpad = cdata[blk].npad;
            dblk.create_restore_point(dpad, lda);
            cdata[blk].d = &d[2*next_elim] - 2*dpad;
            if(dpad || !is_aligned(dblk.aval)) {
               int test = ldlt_tpp_factor(BLOCK_SIZE-dpad, BLOCK_SIZE-dpad,
                     &lperm[dpad],
                     &dblk.aval[dpad*(lda+1)], lda,
                     &cdata[blk].d[2*dpad], thread_work.ld, BLOCK_SIZE,
                     u, small);
               // FIXME: remove following test
               if(test != BLOCK_SIZE-dpad) {
                  printf("Failed DEBUG REMOVE FIXME\n");
                  exit(1);
               }
               int *temp = new int[BLOCK_SIZE];
               for(int i=dpad; i<BLOCK_SIZE; ++i)
                  temp[i] = cdata[blk].perm[lperm[i]];
               for(int i=dpad; i<BLOCK_SIZE; ++i)
                  cdata[blk].perm[i] = temp[i];
               delete[] temp;
            } else {
               block_ldlt<T, BLOCK_SIZE>(dpad, cdata[blk].perm, dblk.aval, lda, cdata[blk].d, thread_work.ld, u, small, lperm);
            }
            // Initialize threshold check (no lock required becuase task depend)
            cdata[blk].npass = BLOCK_SIZE;
         }
         
         // Loop over off-diagonal blocks applying pivot
         for(int jblk=0; jblk<blk; jblk++) {
            #pragma omp task default(none) \
               firstprivate(blk, jblk) \
               shared(blkdata, cdata, lda, global_lperm, global_work) \
               depend(in: blkdata[blk*mblk+blk:1]) \
               depend(inout: blkdata[jblk*mblk+blk:1]) \
               depend(in: cdata[blk:1])
            {
               //printf("ApplyT(%d,%d)\n", blk, jblk);
               BlockData &dblk = blkdata[blk*mblk+blk];
               BlockData &cblk = blkdata[jblk*mblk+blk];
               const int *lperm = &global_lperm[blk*BLOCK_SIZE];
               // Perform necessary operations
               cblk.lwork = global_work.get_wait();
               int rpad = cdata[blk].npad;
               int cpad = cdata[jblk].npad;
               cblk.create_restore_point_with_row_perm(rpad, cpad, lperm, lda);
               cblk.template apply_pivot<OP_T>(cdata[blk].npad, cdata[jblk].nelim, dblk.aval, lda, cdata[blk].d, small, lda);
               // Update threshold check
               int blkpass = cblk.template check_threshold<OP_T>(cdata[blk].npad, cdata[jblk].nelim, u, lda);
               omp_set_lock(&cdata[blk].lock);
               if(blkpass < cdata[blk].npass)
                  cdata[blk].npass = blkpass;
               omp_unset_lock(&cdata[blk].lock);
            }
         }
         for(int iblk=blk+1; iblk<mblk; iblk++) {
            #pragma omp task default(none) \
               firstprivate(blk, iblk) \
               shared(blkdata, cdata, lda, global_lperm, global_work) \
               depend(in: blkdata[blk*mblk+blk:1]) \
               depend(inout: blkdata[blk*mblk+iblk:1]) \
               depend(in: cdata[blk:1])
            {
               //printf("ApplyN(%d,%d)\n", iblk, blk);
               BlockData &dblk = blkdata[blk*mblk+blk];
               BlockData &rblk = blkdata[blk*mblk+iblk];
               const int *lperm = &global_lperm[blk*BLOCK_SIZE];
               // Perform necessary operations
               rblk.lwork = global_work.get_wait();
               int rpad = (iblk < nblk) ? cdata[iblk].npad
                                        : std::max(0, (iblk-nblk+1)*BLOCK_SIZE-(m-n));
               int cpad = cdata[blk].npad;
               rblk.create_restore_point_with_col_perm(rpad, cpad, lperm, lda);
               rblk.template apply_pivot<OP_N>(rpad, cdata[blk].npad, dblk.aval, lda, cdata[blk].d, small, lda);
               // Update threshold check
               int rfrom = (iblk < nblk) ? cdata[iblk].nelim
                                         : std::max(0, (iblk-nblk+1)*BLOCK_SIZE-(m-n));
               int blkpass = rblk.template check_threshold<OP_N>(rfrom, cdata[blk].npad, u, lda);
               omp_set_lock(&cdata[blk].lock);
               if(blkpass < cdata[blk].npass)
                  cdata[blk].npass = blkpass;
               omp_unset_lock(&cdata[blk].lock);
            }
         }

         // Adjust column once all applys have finished and we know final
         // number of passed columns.
         #pragma omp task default(none) \
            firstprivate(blk) \
            shared(blkdata, cdata, changed, next_elim, global_work) \
            depend(inout: cdata[blk:1])
         {
            //printf("Adjust(%d)\n", blk);
            // Adjust to avoid splitting 2x2 pivots
            if(cdata[blk].npass>cdata[blk].npad) {
               T d11 = cdata[blk].d[2*(cdata[blk].npass-1) + 0];
               T d21 = cdata[blk].d[2*(cdata[blk].npass-1) + 1];
               if(d21!=0.0 && d11!=std::numeric_limits<T>::infinity()) {
                  // last passed entry was first part of 2x2
                  cdata[blk].npass--; 
               }
            }
            if(debug) printf("Adjusted to %d\n", cdata[blk].npass);
            // Count threshold
            for(int i=cdata[blk].npad; i<cdata[blk].npass; i++) {
               next_elim++;
               cdata[blk].nelim++;
            }

            // Record if we eliminated anything
            changed = changed || (cdata[blk].npad != cdata[blk].nelim);
         }

         // Update uneliminated columns
         for(int jblk=0; jblk<blk; jblk++) {
            for(int iblk=jblk; iblk<mblk; iblk++) {
               // Calculate block index we depend on for i
               // (we only work with lower half of matrix)
               int iblk_idx = (blk < iblk) ? blk*mblk+iblk
                                           : iblk*mblk+blk;
               #pragma omp task default(none) \
                  firstprivate(blk, iblk, jblk) \
                  shared(cdata, blkdata, lda, all_thread_work, global_work) \
                  depend(inout: blkdata[jblk*mblk+iblk:1]) \
                  depend(in: cdata[blk:1]) \
                  depend(in: blkdata[jblk*mblk+blk:1]) \
                  depend(in: blkdata[iblk_idx:1])
               {
                  //printf("UpdateT(%d,%d,%d)\n", iblk, jblk, blk);
                  // If we're on the block row we've just eliminated, restore
                  // any failed rows and release resources storing checkpoint
                  if(iblk==blk) {
                     if(cdata[blk].nelim < BLOCK_SIZE)
                        blkdata[jblk*mblk+iblk]
                           .restore_part(cdata[blk].nelim, cdata[jblk].nelim,
                                 lda);
                     global_work.release(blkdata[jblk*mblk+blk].lwork);
                  }
                  // Perform actual update (if required)
                  if(cdata[blk].npad != cdata[blk].nelim) {
                     int thread_num = omp_get_thread_num();
                     ThreadWork &thread_work = all_thread_work[thread_num];
                     int const npad = cdata[blk].npad;
                     int nelim = cdata[blk].nelim;
                     int rfrom = (iblk < nblk) ? cdata[iblk].nelim
                                               : std::max(0, (iblk-nblk+1)*BLOCK_SIZE-(m-n));
                     if(blk <= iblk) {
                        calcLD<OP_N>(BLOCK_SIZE-rfrom, nelim-npad,
                           &blkdata[blk*mblk+iblk].aval[npad*lda+rfrom], lda,
                           &cdata[blk].d[2*npad],
                           &thread_work.ld[npad*BLOCK_SIZE+rfrom], BLOCK_SIZE);
                     } else {
                        calcLD<OP_T>(BLOCK_SIZE-rfrom, nelim-npad,
                           &blkdata[iblk*mblk+blk].aval[rfrom*lda+npad], lda,
                           &cdata[blk].d[2*npad],
                           &thread_work.ld[npad*BLOCK_SIZE+rfrom], BLOCK_SIZE);
                     }
                     blkdata[jblk*mblk+iblk].template update<OP_T>(npad, nelim,
                           blkdata[jblk*mblk+blk].aval, lda,
                           thread_work.ld, BLOCK_SIZE,
                           rfrom, cdata[jblk].nelim, lda);
                  }
               }
            }
         }
         for(int jblk=blk; jblk<nblk; jblk++) {
            for(int iblk=jblk; iblk<mblk; iblk++) {
               #pragma omp task default(none) \
                  firstprivate(blk, iblk, jblk) \
                  shared(cdata, blkdata, lda, all_thread_work, global_lperm, \
                         global_work) \
                  depend(inout: blkdata[jblk*mblk+iblk:1]) \
                  depend(in: cdata[blk:1]) \
                  depend(in: blkdata[blk*mblk+iblk:1]) \
                  depend(in: blkdata[blk*mblk+jblk:1])
               {
                  //printf("UpdateN(%d,%d,%d)\n", iblk, jblk, blk);
                  // If we're on the block col we've just eliminated, restore
                  // any failed cols and release checkpoint resources
                  if(jblk==blk) {
                     if(cdata[blk].nelim < BLOCK_SIZE) {
                        if(iblk==blk) {
                           // Diagonal block needs to apply a permutation
                           const int *lperm = &global_lperm[blk*BLOCK_SIZE];
                           blkdata[jblk*mblk+iblk].restore_part_with_sym_perm(
                                 cdata[blk].nelim, lperm, lda
                                 );
                        } else {
                           int rfrom = (iblk < nblk) ? cdata[iblk].nelim
                                                     : std::max(0, (iblk-nblk+1)*BLOCK_SIZE-(m-n));
                           blkdata[jblk*mblk+iblk]
                              .restore_part(rfrom, cdata[blk].nelim, lda);
                        }
                     }
                     global_work.release(blkdata[jblk*mblk+iblk].lwork);
                  }
                  // Perform actual update (if required)
                  if(cdata[blk].npad != cdata[blk].nelim) {
                     int thread_num = omp_get_thread_num();
                     ThreadWork &thread_work = all_thread_work[thread_num];
                     int const npad = cdata[blk].npad;
                     int nelim = cdata[blk].nelim;
                     int rfrom = (iblk < nblk) ? cdata[iblk].nelim
                                               : std::max(0, (iblk-nblk+1)*BLOCK_SIZE-(m-n));
                     calcLD<OP_N>(BLOCK_SIZE-rfrom, nelim-npad,
                           &blkdata[blk*mblk+iblk].aval[npad*lda+rfrom], lda,
                           &cdata[blk].d[2*npad],
                           &thread_work.ld[npad*BLOCK_SIZE+rfrom], BLOCK_SIZE);
                     blkdata[jblk*mblk+iblk].template update<OP_N>(npad, nelim,
                           blkdata[blk*mblk+jblk].aval, lda,
                           thread_work.ld, BLOCK_SIZE,
                           rfrom, cdata[jblk].nelim, lda);
                  }
               }
            }
         }
      }
      #pragma omp taskwait

      delete[] global_lperm;

      return changed;
   }

   static
   void print_mat(int m, int n, const int *perm, const bool *eliminated, const T *a, int lda) {
      for(int row=0; row<m; row++) {
         if(row < n)
            printf("%d%s:", perm[row], eliminated[row]?"X":" ");
         else
            printf("%d%s:", row, "U");
         for(int col=0; col<std::min(n,row+1); col++)
            printf(" %10.4f", a[col*lda+row]);
         printf("\n");
      }
   }

   static
   void print_mat(int mblk, int nblk, int m, int n, const BlockData *blkdata, const struct col_data *cdata, int lda) {
      for(int rblk=0; rblk<mblk; rblk++) {
         int rpad = (rblk < nblk) ? cdata[rblk].npad
                                  : std::max(0, (rblk-nblk+1)*BLOCK_SIZE-(m-n));
         for(int row=rpad; row<BLOCK_SIZE; row++) {
            int r = rblk*BLOCK_SIZE+row;
            if(r < nblk*BLOCK_SIZE)
               printf("%d%s:", cdata[rblk].perm[row], (row<cdata[rblk].nelim)?"X":" ");
            else
               printf("%d%s:", r, "U");
            for(int cblk=0; cblk<std::min(rblk+1,nblk); cblk++) {
               const BlockData &blk = blkdata[cblk*mblk+rblk];
               for(int col=cdata[cblk].npad; col<((rblk==cblk) ? row+1 : BLOCK_SIZE); col++)
                  printf(" %10.4f", blk.aval[col*lda+row]);
            }
            printf("\n");
         }
      }
   }

public:
   CpuLDLT(T u, T small)
   : u(u), small(small)
   {}

   /** Factorize an entire matrix */
   int factor(int m, int n, int *perm, T *a, int lda, T *d) {
      /* Sanity check arguments */
      if(m < n) return -1;
      if(lda < n) return -4;

      /* Initialize useful quantities:
       * If we have m > n, then need to separate diag block and rect part to
       * make handling easier - hence the funny calculation for mblk. */
      int nblk = (n-1) / BLOCK_SIZE + 1;
      int mblk = (m>n) ? nblk + (m-n-1) / BLOCK_SIZE + 1 : nblk;
      int next_elim = 0;

      /* Load data block-wise */
      typedef typename std::allocator_traits<Alloc>::template rebind_alloc<BlockData> BlockDataAlloc;
      BlockDataAlloc bdalloc(alloc);
      BlockData *blkdata = std::allocator_traits<BlockDataAlloc>::allocate(
            bdalloc, mblk*nblk
            );
      for(int i=0; i<mblk*nblk; i++)
         std::allocator_traits<BlockDataAlloc>::construct(
               bdalloc, &blkdata[i]
               );
      for(int jblk=0; jblk<nblk; ++jblk) {
         for(int iblk=0; iblk<mblk; ++iblk) {
            blkdata[jblk*mblk+iblk].aval =
               &a[(jblk*BLOCK_SIZE)*lda + iblk*BLOCK_SIZE];
         }
         // Diagonal block part
         for(int iblk=0; iblk<nblk; iblk++) {
            int roffset = std::max(0, (iblk+1)*BLOCK_SIZE - n);
            int coffset = std::max(0, (jblk+1)*BLOCK_SIZE - n);
            blkdata[jblk*mblk+iblk].aval = 
               &a[(jblk*BLOCK_SIZE-coffset)*lda + iblk*BLOCK_SIZE-roffset];
         }
         // Rectangular block below it
         T *arect = &a[n];
         for(int iblk=0; iblk<mblk-nblk; iblk++) {
            int roffset = std::max(0, (iblk+1)*BLOCK_SIZE - (m-n));
            int coffset = std::max(0, (jblk+1)*BLOCK_SIZE - n);
            blkdata[jblk*mblk+nblk+iblk].aval =
                  &arect[(jblk*BLOCK_SIZE-coffset)*lda + iblk*BLOCK_SIZE-roffset];
         }
      }

      /* Temporary workspaces */
      struct col_data *cdata = new struct col_data[nblk];

      /* Load column data */
      for(int blk=0; blk<nblk-1; blk++) {
         cdata[blk].perm = &perm[blk*BLOCK_SIZE];
      }
      {
         // Handle last block specially to allow for undersize
         int coffset = nblk*BLOCK_SIZE - n;
         cdata[nblk-1].perm = &perm[(nblk-1)*BLOCK_SIZE] - coffset;
      }
      if(n < nblk*BLOCK_SIZE) {
         // Account for extra cols as "already eliminated"
         cdata[nblk-1].npad = nblk*BLOCK_SIZE - n;
         cdata[nblk-1].nelim = nblk*BLOCK_SIZE - n;
      }

      /* Main loop
       *    - Each pass leaves any failed pivots in place and keeps everything
       *      up-to-date.
       *    - If no pivots selected across matrix, perform swaps to get large
       *      entries into diagonal blocks
       */
      int num_threads = omp_get_max_threads();
      ThreadWork all_thread_work[num_threads];
      // FIXME: Following line is a maximum! Make smaller?
      BlockPool<T, BLOCK_SIZE> global_work((nblk*(nblk+1))/2+mblk*nblk);
      run_elim(next_elim, m, n, mblk, nblk, cdata, blkdata, d, lda, global_work, all_thread_work);

      // Calculate number of successful eliminations (removing any dummy cols)
      int num_elim = next_elim;

      // Permute failed entries to end
      int* failed_perm = new int[n - num_elim];
      for(int jblk=0, insert=0, fail_insert=0; jblk<nblk; jblk++) {
         cdata[jblk].move_back(&perm[insert], &failed_perm[fail_insert]);
         insert += cdata[jblk].nelim;
         fail_insert += BLOCK_SIZE - cdata[jblk].nelim;
      }
      for(int i=0; i<n-num_elim; ++i)
         perm[num_elim+i] = failed_perm[i];
      delete[] failed_perm;

      // Extract failed entries of a
      int nfail = n-num_elim;
      T* failed_diag = new T[nfail*n];
      T* failed_rect = new T[nfail*(m-n)];
      for(int jblk=0, jfail=0, jinsert=0; jblk<nblk; ++jblk) {
         for(int iblk=jblk, ifail=jfail, iinsert=jinsert; iblk<nblk; ++iblk) {
            blkdata[jblk*mblk+iblk].copy_failed_diag(
                  cdata[iblk], cdata[jblk],
                  &failed_diag[jinsert*nfail+ifail],
                  &failed_diag[iinsert*nfail+jfail],
                  &failed_diag[num_elim*nfail+jfail*nfail+ifail],
                  nfail, lda
                  );
            iinsert += cdata[iblk].nelim;
            ifail += BLOCK_SIZE - cdata[iblk].nelim;
         }
         for(int iblk=nblk; iblk<mblk; ++iblk) {
            int nrow = std::min(BLOCK_SIZE, m-n - (iblk-nblk)*BLOCK_SIZE);
            blkdata[jblk*mblk+iblk].copy_failed_rect(
                  nrow, cdata[jblk],
                  &failed_rect[jfail*(m-n)+(iblk-nblk)*BLOCK_SIZE], m-n, lda
                  );
         }
         jinsert += cdata[jblk].nelim;
         jfail += BLOCK_SIZE - cdata[jblk].nelim;
      }

      // Move data up
      for(int jblk=0, jinsert=0; jblk<nblk; ++jblk) {
         for(int iblk=jblk, iinsert=jinsert; iblk<nblk; ++iblk) {
            blkdata[jblk*mblk+iblk].move_up_diag(cdata[iblk], cdata[jblk],
                  &a[jinsert*lda+iinsert], lda, lda);
            iinsert += cdata[iblk].nelim;
         }
         for(int iblk=nblk; iblk<mblk; ++iblk) {
            int rfrom = std::max(0, (iblk-nblk+1)*BLOCK_SIZE-(m-n));
            blkdata[jblk*mblk+iblk].move_up_rect(rfrom, cdata[jblk],
                  &a[jinsert*lda+n+(iblk-nblk)*BLOCK_SIZE], lda, lda);
         }
         jinsert += cdata[jblk].nelim;
      }
      
      // Store failed entries back to correct locations
      // Diagonal part
      for(int j=0; j<n; ++j)
      for(int i=std::max(j,num_elim), k=i-num_elim; i<n; ++i, ++k)
         a[j*lda+i] = failed_diag[j*nfail+k];
      // Rectangular part
      T* arect = &a[num_elim*lda+n];
      for(int j=0; j<nfail; ++j)
      for(int i=0; i<m-n; ++i)
         arect[j*lda+i] = failed_rect[j*(m-n)+i];
      delete[] failed_diag;
      delete[] failed_rect;

      if(debug) {
         bool *eliminated = new bool[n];
         for(int i=0; i<num_elim; i++) eliminated[i] = true;
         for(int i=num_elim; i<n; i++) eliminated[i] = false;
         printf("FINAL:\n");
         print_mat(m, n, perm, eliminated, a, lda);
         delete[] eliminated;
      }
      
      // Free memory
      delete[] cdata;
      for(int i=0; i<mblk*nblk; i++)
         std::allocator_traits<BlockDataAlloc>::destroy(
               bdalloc, &blkdata[i]
               );
      std::allocator_traits<BlockDataAlloc>::deallocate(
            bdalloc, blkdata, mblk*nblk
            );

      return num_elim;
   }
};

} /* namespace spral::ssids::cpu::ldlt_app */

template<typename T>
int ldlt_app_factor(int m, int n, int *perm, T *a, int lda, T *d, double u, double small) {
   return ldlt_app::CpuLDLT<T>(u, small).factor(m, n, perm, a, lda, d);
}
template int ldlt_app_factor<double>(int, int, int*, double*, int, double*, double, double);

template <typename T>
void ldlt_app_solve_fwd(int m, int n, T const* l, int ldl, int nrhs, T* x, int ldx) {
   if(nrhs==1) {
      host_trsv(FILL_MODE_LWR, OP_N, DIAG_UNIT, n, l, ldl, x, 1);
      if(m > n)
         gemv(OP_N, m-n, n, -1.0, &l[n], ldl, x, 1, 1.0, &x[n], 1);
   } else {
      host_trsm(SIDE_LEFT, FILL_MODE_LWR, OP_N, DIAG_UNIT, n, nrhs, 1.0, l, ldl, x, ldx);
      if(m > n)
         host_gemm(OP_N, OP_N, m-n, nrhs, n, -1.0, &l[n], ldl, x, ldx, 1.0, &x[n], ldx);
   }
}
template void ldlt_app_solve_fwd<double>(int, int, double const*, int, int, double*, int);

template <typename T>
void ldlt_app_solve_diag(int n, T const* d, T* x) {
   for(int i=0; i<n; ) {
      if(i+1==n || std::isfinite(d[2*i+2])) {
         // 1x1 pivot
         T d11 = d[2*i];
         x[i] *= d11;
         i++;
      } else {
         // 2x2 pivot
         T d11 = d[2*i];
         T d21 = d[2*i+1];
         T d22 = d[2*i+3];
         T x1 = x[i];
         T x2 = x[i+1];
         x[i]   = d11*x1 + d21*x2;
         x[i+1] = d21*x1 + d22*x2;
         i += 2;
      }
   }
}
template void ldlt_app_solve_diag<double>(int, double const*, double*);

template <typename T>
void ldlt_app_solve_bwd(int m, int n, T const* l, int ldl, int nrhs, T* x, int ldx) {
   if(nrhs==1) {
      if(m > n)
         gemv(OP_T, m-n, n, -1.0, &l[n], ldl, &x[n], 1, 1.0, x, 1);
      host_trsv(FILL_MODE_LWR, OP_T, DIAG_UNIT, n, l, ldl, x, 1);
   } else {
      if(m > n)
         host_gemm(OP_T, OP_N, n, nrhs, m-n, -1.0, &l[n], ldl, &x[n], ldx, 1.0, x, ldx);
      host_trsm(SIDE_LEFT, FILL_MODE_LWR, OP_T, DIAG_UNIT, n, nrhs, 1.0, l, ldl, x, ldx);
   }
}
template void ldlt_app_solve_bwd<double>(int, int, double const*, int, int, double*, int);

}}} /* namespaces spral::ssids::cpu */