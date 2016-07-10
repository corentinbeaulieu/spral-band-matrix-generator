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

#include <cstddef>

namespace spral { namespace ssids { namespace cpu {

struct cpu_factor_options {
   double small;
   double u;
   int print_level;
   int cpu_small_subtree_threshold;
   int cpu_task_block_size;
};

struct cpu_factor_stats {
   int flag;
   int num_delay;
   int num_neg;
   int num_two;
   int num_zero;
   int maxfront;
   int elim_at_pass[5];
   int elim_at_itr[5];
};

}}} /* namespaces spral::ssids::cpu */

extern "C" {

double *spral_ssids_smalloc_dbl(void *alloc, size_t sz);
int *spral_ssids_smalloc_int(void *alloc, size_t sz);

} /* end extern "C" */
