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
#pragma once

/* Standard headers */
#include <cstddef>
#include <sstream>
#include <stdexcept>
/* SPRAL headers */
#include "cpu_iface.hxx"
#include "kernels/assemble.hxx"
#include "kernels/cholesky.hxx"
#include "kernels/CpuLDLT.cxx"

namespace spral { namespace ssids { namespace cpu {

const int SSIDS_SUCCESS = 0;
const int SSIDS_ERROR_NOT_POS_DEF = -6;

/** A Workspace is a chunk of memory that can be reused. The get_ptr<T>(len)
 * function provides a pointer to it after ensuring it is of at least the
 * given size. */
class Workspace {
public:
   Workspace(size_t sz)
      : mem_(::operator new(sz)), sz_(sz)
      {}
   ~Workspace() {
      ::operator delete(mem_);
   }
   template <typename T>
   T* get_ptr(size_t len) {
      if(sz_ < len*sizeof(T)) {
         // Need to resize
         ::operator delete(mem_);
         sz_ = len*sizeof(T);
         mem_ = ::operator new(sz_);
      }
      return static_cast<T*>(mem_);
   }
private:
   void* mem_;
   size_t sz_;
};

/* Factorize a node (indef) */
template <typename T, int BLOCK_SIZE>
void factor_node_indef(
      int ni, // FIXME: remove post debug
      SymbolicNode const& snode,
      NumericNode<T>* node,
      struct cpu_factor_options const* options,
      struct cpu_factor_stats& stats
      ) {
   /* Extract useful information about node */
   int m = snode.nrow + node->ndelay_in;
   int n = snode.ncol + node->ndelay_in;
   T *lcol = node->lcol;
   T *d = &node->lcol[ ((long) m)*n ];
   int *perm = node->perm;

   /* Perform factorization */
   typedef CpuLDLT<T, BLOCK_SIZE> CpuLDLTSpec;
   //typedef CpuLDLT<T, BLOCK_SIZE, 5, true> CpuLDLTSpecDebug; // FIXME: debug remove
   struct CpuLDLTSpec::stat_type bubstats; // FIXME: not needed?
   node->nelim = CpuLDLTSpec(options->u, options->small).factor(m, n, perm, lcol, m, d, &bubstats);
   for(int i=0; i<5; i++) {
      stats.elim_at_pass[i] += bubstats.elim_at_pass[i];
   }
   int last_remain = n;
   for(int i=0; i<bubstats.nitr; i++) {
      stats.elim_at_itr[i] += last_remain - bubstats.remain[i];
      last_remain = bubstats.remain[i];
   }
   /*if(bubstats.nitr > 2) {
      printf("Node %d: %dx%d delay %d nitr %d\n", ni, m, n, n-node->nelim, bubstats.nitr);
      for(int i=0; i<bubstats.nitr; i++)
         printf("--> itr %d passes %d remain %d\n", i, bubstats.npass[i], bubstats.remain[i]);
   }*/

   /*for(int i=node->nelim; i<m; i++) {
      printf("%d:", i);
      for(int j=node->nelim; j<n; j++)
         printf(" %10.2e", lcol[j*m+i]);
      printf("\n");
   }*/

   /* Record information */
   node->ndelay_out = n - node->nelim;
   stats.num_delay += node->ndelay_out;
}
/* Factorize a node (posdef) */
template <typename T, int BLOCK_SIZE>
void factor_node_posdef(
      SymbolicNode const& snode,
      NumericNode<T>* node,
      struct cpu_factor_options const* options,
      struct cpu_factor_stats& stats
      ) {
   /* Extract useful information about node */
   int m = snode.nrow;
   int n = snode.ncol;
   T *lcol = node->lcol;
   T *contrib = node->contrib;

   /* Perform factorization */
   int flag;
   cholesky_factor(m, n, lcol, m, 1.0, contrib, m-n, options->cpu_task_block_size, &flag);
   #pragma omp taskwait
   if(flag!=-1) {
      node->nelim = flag+1;
      stats.flag = SSIDS_ERROR_NOT_POS_DEF;
      return;
   }
   node->nelim = n;

   /* Record information */
   node->ndelay_out = 0;
}
/* Factorize a node (wrapper) */
template <bool posdef, int BLOCK_SIZE, typename T>
void factor_node(
      int ni,
      SymbolicNode const& snode,
      NumericNode<T>* node,
      struct cpu_factor_options const* options,
      struct cpu_factor_stats& stats
      ) {
   if(posdef) factor_node_posdef<T, BLOCK_SIZE>(snode, node, options, stats);
   else       factor_node_indef <T, BLOCK_SIZE>(ni, snode, node, options, stats);
}

/* Calculate update */
template <typename T, typename ContribAlloc>
void calculate_update(
      SymbolicNode const& snode,
      NumericNode<T>* node,
      ContribAlloc& contrib_alloc,
      Workspace& work
      ) {
   typedef std::allocator_traits<ContribAlloc> CATraits;

   // Check there is work to do
   int m = snode.nrow - snode.ncol;
   int n = node->nelim;
   if(n==0 && !node->first_child) {
      // If everything is delayed, and no contribution from children then
      // free contrib memory and mark as no contribution for parent's assembly
      // FIXME: actually loop over children and check one exists with contriub
      //        rather than current approach of just looking for children.
      CATraits::deallocate(contrib_alloc, node->contrib, m*m);
      node->contrib = NULL;
      return;
   }
   if(m==0 || n==0) return; // no-op

   // Indefinte - need to recalculate LD before we can use it!

   // Calculate LD
   T *lcol = &node->lcol[snode.ncol+node->ndelay_in];
   int ldl = snode.nrow + node->ndelay_in;
   T *d = &node->lcol[ldl*(snode.ncol+node->ndelay_in)];
   T *ld = work.get_ptr<T>(m*n);
   for(int j=0; j<n;) {
      if(d[2*j+1] == 0.0) {
         // 1x1 pivot
         // (Actually stored as D^-1 so need to invert it again)
         if(d[2*j] == 0.0) {
            // Handle zero pivots with care
            for(int i=0; i<m; i++) {
               ld[j*m+i] = 0.0;
            }
         } else {
            // Standard 1x1 pivot
            T d11 = 1/d[2*j];
            // And calulate ld
            for(int i=0; i<m; i++) {
               ld[j*m+i] = d11*lcol[j*ldl+i];
            }
         }
         // Increment j
         j++;
      } else {
         // 2x2 pivot
         // (Actually stored as D^-1 so need to invert it again)
         T di11 = d[2*j]; T di21 = d[2*j+1]; T di22 = d[2*j+3];
         T det = di11*di22 - di21*di21;
         T d11 = di22 / det; T d21 = -di21 / det; T d22 = di11 / det;
         // And calulate ld
         for(int i=0; i<m; i++) {
            ld[j*m+i]     = d11*lcol[j*ldl+i] + d21*lcol[(j+1)*ldl+i];
            ld[(j+1)*m+i] = d21*lcol[j*ldl+i] + d22*lcol[(j+1)*ldl+i];
         }
         // Increment j
         j += 2;
      }
   }

   // Apply update to contrib block
   host_gemm<T>(OP_N, OP_T, m, m, n,
         -1.0, lcol, ldl, ld, m,
         1.0, node->contrib, m);

   // FIXME: debug remove
   /*printf("Contrib = \n");
   for(int i=0; i<m; i++) {
      for(int j=0; j<m; j++) printf(" %e", node->contrib[j*m+i]);
      printf("\n");
   }*/
}

}}} /* end of namespace spral::ssids::cpu */
