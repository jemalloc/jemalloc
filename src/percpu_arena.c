#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/jemalloc_internal_externs.h"
#include "jemalloc/internal/percpu_arena.h"

/******************************************************************************/
/* Data. */

/*
 * Define names for both uninitialized and initialized phases, so that
 * options and mallctl processing are straightforward.
 */
const char *const percpu_arena_mode_names[] = {
    "percpu", "phycpu", "disabled", "percpu", "phycpu"};
percpu_arena_mode_t opt_percpu_arena = PERCPU_ARENA_DEFAULT;

/******************************************************************************/
