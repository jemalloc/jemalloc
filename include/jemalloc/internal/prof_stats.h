#ifndef JEMALLOC_INTERNAL_PROF_STATS_H
#define JEMALLOC_INTERNAL_PROF_STATS_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/mutex.h"

typedef struct prof_stats_s prof_stats_t;
struct prof_stats_s {
	uint64_t req_sum;
	uint64_t count;
};

extern malloc_mutex_t prof_stats_mtx;

void prof_stats_inc(tsd_t *tsd, szind_t ind, size_t size);
void prof_stats_dec(tsd_t *tsd, szind_t ind, size_t size);
void prof_stats_get_live(tsd_t *tsd, szind_t ind, prof_stats_t *stats);
void prof_stats_get_accum(tsd_t *tsd, szind_t ind, prof_stats_t *stats);

extern atomic_u64_t prof_backtrace_count;
extern atomic_u64_t prof_backtrace_time_ns;
#ifdef JEMALLOC_PROF_FRAME_POINTER
extern atomic_u64_t prof_stack_range_count;
extern atomic_u64_t prof_stack_range_time_ns;
#endif

#endif /* JEMALLOC_INTERNAL_PROF_STATS_H */
