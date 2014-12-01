!
! This module defines ssids_fkeep type and associated procedures (CPU version)
!
module spral_ssids_fkeep
   use spral_ssids_alloc, only : smfreeall
   use spral_ssids_datatypes, only : long, node_type, smalloc_type, &
                                     ssids_akeep, ssids_options, ssids_inform, &
                                     wp, SSIDS_ERROR_ALLOCATION, &
                                     SSIDS_SOLVE_JOB_ALL, SSIDS_SOLVE_JOB_BWD, &
                                     SSIDS_SOLVE_JOB_DIAG, SSIDS_SOLVE_JOB_FWD,&
                                     SSIDS_SOLVE_JOB_DIAG_BWD
   use spral_ssids_solve_cpu, only : inner_solve, subtree_bwd_solve
   use, intrinsic :: iso_c_binding
   implicit none

   private
   public :: ssids_fkeep

   !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

   !
   ! Data type for data generated in factorise phase
   !
   type ssids_fkeep
      integer :: flag ! copy of error flag.
      real(wp), dimension(:), allocatable :: scaling ! Stores scaling for
         ! each entry (in original matrix order)
      type(node_type), dimension(:), allocatable :: nodes ! Stores pointers
         ! to information about nodes
      type(smalloc_type), pointer :: alloc=>null() ! Linked list of memory pages
         ! pointed to by nodes variable
      logical :: pos_def ! set to true if user indicates matrix pos. definite

      ! Info components to copy on solve
      integer :: matrix_rank
      integer :: maxfront
      integer :: num_delay
      integer(long) :: num_factor
      integer(long) :: num_flops
      integer :: num_neg
      integer :: num_two

   contains
      procedure, pass(fkeep) :: inner_factor => inner_factor_cpu ! Do actual factorization
      procedure, pass(fkeep) :: inner_solve => inner_solve_cpu ! Do actual solve
      procedure, pass(fkeep) :: free => free_fkeep ! Frees memory
   end type ssids_fkeep

contains

subroutine inner_factor_cpu(fkeep, akeep, val, options, inform)
   class(ssids_akeep), intent(in) :: akeep
   class(ssids_fkeep), intent(inout) :: fkeep
   real(wp), dimension(*), target, intent(in) :: val
   class(ssids_options), intent(in) :: options
   class(ssids_inform), intent(inout) :: inform

   print *, "CPU Factor not implemented yet!"
   stop
end subroutine inner_factor_cpu

subroutine inner_solve_cpu(local_job, nrhs, x, ldx, akeep, fkeep, options, inform)
   class(ssids_akeep), intent(in) :: akeep
   class(ssids_fkeep), intent(inout) :: fkeep
   integer, intent(inout) :: local_job
   integer, intent(in) :: nrhs
   real(wp), dimension(ldx,nrhs), target, intent(inout) :: x
   integer, intent(in) :: ldx
   type(ssids_options), intent(in) :: options
   type(ssids_inform), intent(inout) :: inform

   integer :: i, r
   integer :: n

   n = akeep%n

   if (allocated(fkeep%scaling)) then
      if (local_job == SSIDS_SOLVE_JOB_ALL .or. &
            local_job == SSIDS_SOLVE_JOB_FWD) then
         do r = 1, nrhs
            !x(1:n,r) = x(1:n,r) * fkeep%scaling(1:n)
            do i = 1, n
               x(akeep%invp(i),r) = x(akeep%invp(i),r) * fkeep%scaling(i)
            end do
         end do
      end if
   end if

   ! Perform supernodal forward solve or diagonal solve (both in serial)
   call inner_solve(fkeep%pos_def, local_job, akeep%nnodes, &
      fkeep%nodes, akeep%sptr, akeep%rptr, akeep%rlist, akeep%invp, nrhs, &
      x, ldx, inform%stat)
   if (inform%stat .ne. 0) goto 100

   if( local_job.eq.SSIDS_SOLVE_JOB_DIAG_BWD .or. &
         local_job.eq.SSIDS_SOLVE_JOB_BWD .or. &
         local_job.eq.SSIDS_SOLVE_JOB_ALL ) then
      call subtree_bwd_solve(akeep%nnodes, 1, local_job, fkeep%pos_def,  &
         akeep%nnodes, fkeep%nodes, akeep%sptr, akeep%rptr, akeep%rlist, &
         akeep%invp, nrhs, x, ldx, inform%stat)
   end if
   if (inform%stat .ne. 0) goto 100

   if (allocated(fkeep%scaling)) then
      if (local_job == SSIDS_SOLVE_JOB_ALL .or. &
            local_job == SSIDS_SOLVE_JOB_BWD .or. &
            local_job == SSIDS_SOLVE_JOB_DIAG_BWD) then
         do r = 1, nrhs
            !x(1:n,r) = x(1:n,r) * fkeep%scaling(1:n)
            do i = 1, n
               x(akeep%invp(i),r) = x(akeep%invp(i),r) * fkeep%scaling(i)
            end do
         end do
      end if
   end if

   return

   100 continue
   inform%flag = SSIDS_ERROR_ALLOCATION
   return

end subroutine inner_solve_cpu

subroutine free_fkeep(fkeep, flag)
   class(ssids_fkeep), intent(inout) :: fkeep
   integer, intent(out) :: flag ! not actually used for cpu version, set to 0

   integer :: st, i

   flag = 0 ! Not used for basic SSIDS, just zet to zero

   ! Skip if nothing intialized
   if (.not.allocated(fkeep%nodes)) return

   ! Free memory
   call smfreeall(fkeep%alloc)
   deallocate(fkeep%alloc)
   nullify(fkeep%alloc)

   deallocate(fkeep%nodes, stat=st)
   deallocate(fkeep%scaling, stat=st)

end subroutine free_fkeep

end module spral_ssids_fkeep
