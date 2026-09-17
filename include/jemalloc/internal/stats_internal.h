#ifndef JEMALLOC_INTERNAL_STATS_INTERNAL_H
#define JEMALLOC_INTERNAL_STATS_INTERNAL_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/arena.h" /* ARENA_NAME_LEN */
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/ctl.h"
#include "jemalloc/internal/emitter.h"
#include "jemalloc/internal/prof_stats.h"

/*
 * Internal implementation support for src/stats.c only -- do NOT include from
 * other translation units.
 *
 * This header holds noisy local scaffolding for malloc_stats_print(): ctl
 * gather plumbing, gather structs, emitter table-column helpers, and option
 * list helpers.  It is deliberately not a reusable stats-gather API.
 */

#define CTL_GET(n, v, t)                                                       \
	do {                                                                   \
		size_t sz = sizeof(t);                                         \
		xmallctl(n, (void *)v, &sz, NULL, 0);                          \
	} while (0)

#define CTL_LEAF_PREPARE(mib, miblen, name)                                    \
	do {                                                                   \
		assert(miblen < CTL_MAX_DEPTH);                                \
		size_t miblen_new = CTL_MAX_DEPTH;                             \
		xmallctlmibnametomib(mib, miblen, name, &miblen_new);          \
		assert(miblen_new > miblen);                                   \
	} while (0)

#define CTL_LEAF(mib, miblen, leaf, v, t)                                      \
	do {                                                                   \
		assert(miblen < CTL_MAX_DEPTH);                                \
		size_t miblen_new = CTL_MAX_DEPTH;                             \
		size_t sz = sizeof(t);                                         \
		xmallctlbymibname(                                             \
		    mib, miblen, leaf, &miblen_new, (void *)v, &sz, NULL, 0);  \
		assert(miblen_new == miblen + 1);                              \
	} while (0)

#define CTL_MIB_GET(n, i, v, t, ind)                                           \
	do {                                                                   \
		size_t mib[CTL_MAX_DEPTH];                                     \
		size_t miblen = sizeof(mib) / sizeof(size_t);                  \
		size_t sz = sizeof(t);                                         \
		xmallctlnametomib(n, mib, &miblen);                            \
		mib[(ind)] = (i);                                              \
		xmallctlbymib(mib, miblen, (void *)v, &sz, NULL, 0);           \
	} while (0)

#define CTL_M1_GET(n, i, v, t) CTL_MIB_GET(n, i, v, t, 1)
#define CTL_M2_GET(n, i, v, t) CTL_MIB_GET(n, i, v, t, 2)

/******************************************************************************/
/*
 * Per-section gather structs.  The stats_gather_*() functions that fill them
 * are static in src/stats.c.
 */

/* One arena's memory-accounting stats (stats.arenas.<i>.*). */
typedef struct stats_arena_mem_s {
	size_t mapped;
	size_t retained;
	size_t pinned;
	size_t base;
	size_t internal;
	size_t metadata_edata;
	size_t metadata_rtree;
	size_t metadata_thp;
	size_t tcache_bytes;
	size_t tcache_stashed_bytes;
	size_t resident;
	size_t abandoned_vm;
	size_t extent_avail;
	size_t page; /* arenas.page, for the active-bytes conversion */
} stats_arena_mem_t;

/* One arena extents row (stats.arenas.<i>.extents.<pszind>.*). */
typedef struct stats_arena_extent_s {
	size_t ndirty;
	size_t nmuzzy;
	size_t nretained;
	size_t npinned;
	size_t ntotal;
	size_t dirty_bytes;
	size_t muzzy_bytes;
	size_t retained_bytes;
	size_t pinned_bytes;
	size_t total_bytes;
} stats_arena_extent_t;

/* One arena's "basics" (stats.arenas.<i>.{nthreads,uptime,dss}). */
typedef struct stats_arena_basics_s {
	unsigned    nthreads;
	uint64_t    uptime;
	const char *dss;
	char        name[ARENA_NAME_LEN];
	bool        has_name; /* false for the merged/destroyed pseudo-arenas */
} stats_arena_basics_t;

/* One arena's decay / page stats (stats.arenas.<i>.*). */
typedef struct stats_arena_decay_s {
	ssize_t  dirty_decay_ms;
	ssize_t  muzzy_decay_ms;
	size_t   pactive;
	size_t   pdirty;
	size_t   pmuzzy;
	uint64_t dirty_npurge;
	uint64_t dirty_nmadvise;
	uint64_t dirty_purged;
	uint64_t muzzy_npurge;
	uint64_t muzzy_nmadvise;
	uint64_t muzzy_purged;
} stats_arena_decay_t;

/* One size class of arena allocation counts (stats.arenas.<i>.{small,large}.*). */
typedef struct stats_arena_alloc_s {
	size_t   allocated;
	uint64_t nmalloc;
	uint64_t ndalloc;
	uint64_t nrequests;
	uint64_t nfills;
	uint64_t nflushes;
} stats_arena_alloc_t;

/*
 * One arena bin row (stats.arenas.<i>.bins.<j>.* + arenas.bin.<j>.*,
 * plus prof.stats.bins.<j>.{live,accum} when profiling stats are enabled).
 */
typedef struct stats_arena_bin_s {
	uint64_t nslabs;
	uint64_t nmalloc;
	uint64_t ndalloc;
	uint64_t nrequests;
	uint64_t nfills;
	uint64_t nflushes;
	uint64_t nreslabs;
	size_t   reg_size;
	size_t   slab_size;
	size_t   curregs;
	size_t   curslabs;
	size_t   nonfull_slabs;
	uint32_t nregs;
	uint32_t nshards;
	prof_stats_t prof_live;
	prof_stats_t prof_accum;
} stats_arena_bin_t;

/*
 * One arena large-extent row (stats.arenas.<i>.lextents.<j>.* +
 * arenas.lextent.<j>.*, plus prof.stats.lextents.<j>.{live,accum} when
 * profiling stats are enabled).
 */
typedef struct stats_arena_lextent_s {
	uint64_t nmalloc;
	uint64_t ndalloc;
	uint64_t nrequests;
	size_t   lextent_size;
	size_t   curlextents;
	prof_stats_t prof_live;
	prof_stats_t prof_accum;
} stats_arena_lextent_t;

/* HPA shard small-extent cache (stats.arenas.<i>.hpa_sec_*). */
typedef struct stats_arena_hpa_sec_s {
	size_t sec_bytes;
	size_t sec_hits;
	size_t sec_misses;
	size_t sec_dalloc_flush;
	size_t sec_dalloc_noflush;
	size_t sec_overfills;
} stats_arena_hpa_sec_t;

/* PAC small-extent cache (stats.arenas.<i>.pac_sec_*). */
typedef struct stats_arena_pac_sec_s {
	size_t sec_bytes;
	size_t sec_hits;
	size_t sec_misses;
	size_t sec_dalloc_flush;
	size_t sec_dalloc_noflush;
} stats_arena_pac_sec_t;

/* HPA shard counters (stats.arenas.<i>.hpa_shard.*). */
typedef struct stats_arena_hpa_counters_s {
	size_t npageslabs;
	size_t nactive;
	size_t ndirty;
	size_t npageslabs_nonhuge;
	size_t nactive_nonhuge;
	size_t ndirty_nonhuge;
	size_t nretained_nonhuge;
	size_t npageslabs_huge;
	size_t nactive_huge;
	size_t ndirty_huge;
	uint64_t npurge_passes;
	uint64_t npurges;
	uint64_t nhugifies;
	uint64_t nhugify_failures;
	uint64_t ndehugifies;
} stats_arena_hpa_counters_t;

/* One HPA slab class (full/empty/nonfull) row. */
typedef struct stats_arena_hpa_slab_s {
	size_t npageslabs_huge;
	size_t nactive_huge;
	size_t ndirty_huge;
	size_t npageslabs_nonhuge;
	size_t nactive_nonhuge;
	size_t ndirty_nonhuge;
	size_t nretained_nonhuge;
} stats_arena_hpa_slab_t;

/* Arena configuration / size-class scalars (arenas.*). */
typedef struct stats_arena_config_s {
	unsigned narenas;
	ssize_t  dirty_decay_ms;
	ssize_t  muzzy_decay_ms;
	size_t   quantum;
	size_t   page;
	size_t   hugepage;
	bool     have_tcache_max;
	size_t   tcache_max;
	unsigned nbins;
	unsigned nhbins;
	unsigned nlextents;
} stats_arena_config_t;

/* Per-size-class geometry (emitted in JSON only). */
typedef struct stats_arena_bin_meta_s {
	size_t   size;
	uint32_t nregs;
	size_t   slab_size;
	uint32_t nshards;
} stats_arena_bin_meta_t;

typedef struct stats_arena_lextent_meta_s {
	size_t size;
} stats_arena_lextent_meta_t;

/* Process-wide global stats (stats.* + stats.background_thread.*). */
typedef struct stats_global_s {
	size_t   allocated;
	size_t   active;
	size_t   metadata;
	size_t   metadata_edata;
	size_t   metadata_rtree;
	size_t   metadata_thp;
	size_t   resident;
	size_t   mapped;
	size_t   retained;
	size_t   pinned;
	size_t   zero_reallocs;
	size_t   num_background_threads;
	uint64_t background_thread_num_runs;
	uint64_t background_thread_run_interval;
} stats_global_t;

/******************************************************************************/
/*
 * Emitter table-column helper macros: declare an emitter_col_t (and, for
 * COL_HDR, its header column) and wire it into a row.
 */

#define COL_DECLARE(column_name) emitter_col_t col_##column_name;

#define COL_INIT(row_name, column_name, left_or_right, col_width, etype)       \
	emitter_col_init(&col_##column_name, &row_name);                       \
	col_##column_name.justify = emitter_justify_##left_or_right;           \
	col_##column_name.width = col_width;                                   \
	col_##column_name.type = emitter_type_##etype;

#define COL(row_name, column_name, left_or_right, col_width, etype)            \
	COL_DECLARE(column_name);                                              \
	COL_INIT(row_name, column_name, left_or_right, col_width, etype)

#define COL_HDR_DECLARE(column_name)                                           \
	COL_DECLARE(column_name);                                              \
	emitter_col_t header_##column_name;

#define COL_HDR_INIT(                                                          \
    row_name, column_name, human, left_or_right, col_width, etype)             \
	COL_INIT(row_name, column_name, left_or_right, col_width, etype)       \
	emitter_col_init(&header_##column_name, &header_##row_name);           \
	header_##column_name.justify = emitter_justify_##left_or_right;        \
	header_##column_name.width = col_width;                                \
	header_##column_name.type = emitter_type_title;                        \
	header_##column_name.str_val = human ? human : #column_name;

#define COL_HDR(row_name, column_name, human, left_or_right, col_width, etype) \
	COL_HDR_DECLARE(column_name)                                           \
	COL_HDR_INIT(                                                          \
	    row_name, column_name, human, left_or_right, col_width, etype)

/*
 * stats_general_print() config/option list helpers.  Unhygienic by design:
 * they assume `emitter` and the standard scratch locals are in scope at the
 * call site -- bv/bsz (bool), cpv/cpsz (const char *), uv/usz (unsigned),
 * i64v/i64sz, u64v/u64sz, sv/ssz (size_t), ssv/sssz (ssize_t), plus bv2/ssv2
 * for the *_MUTABLE variants.  (CONFIG_WRITE_BOOL uses CTL_GET above; the
 * OPT_WRITE_* variants use je_mallctl directly.)
 */
#define CONFIG_WRITE_BOOL(name)                                                \
	do {                                                                   \
		CTL_GET("config." #name, &bv, bool);                           \
		emitter_kv(                                                    \
		    emitter, #name, "config." #name, emitter_type_bool, &bv);  \
	} while (0)

#define OPT_WRITE(name, var, size, emitter_type)                               \
	if (je_mallctl("opt." name, (void *)&var, &size, NULL, 0) == 0) {      \
		emitter_kv(emitter, name, "opt." name, emitter_type, &var);    \
	}

#define OPT_WRITE_MUTABLE(name, var1, var2, size, emitter_type, altname)       \
	if (je_mallctl("opt." name, (void *)&var1, &size, NULL, 0) == 0        \
	    && je_mallctl(altname, (void *)&var2, &size, NULL, 0) == 0) {      \
		emitter_kv_note(emitter, name, "opt." name, emitter_type,      \
		    &var1, altname, emitter_type, &var2);                      \
	}

#define OPT_WRITE_BOOL(name) OPT_WRITE(name, bv, bsz, emitter_type_bool)
#define OPT_WRITE_BOOL_MUTABLE(name, altname)                                  \
	OPT_WRITE_MUTABLE(name, bv, bv2, bsz, emitter_type_bool, altname)

#define OPT_WRITE_UNSIGNED(name) OPT_WRITE(name, uv, usz, emitter_type_unsigned)

#define OPT_WRITE_INT64(name) OPT_WRITE(name, i64v, i64sz, emitter_type_int64)
#define OPT_WRITE_UINT64(name) OPT_WRITE(name, u64v, u64sz, emitter_type_uint64)

#define OPT_WRITE_SIZE_T(name) OPT_WRITE(name, sv, ssz, emitter_type_size)
#define OPT_WRITE_SSIZE_T(name) OPT_WRITE(name, ssv, sssz, emitter_type_ssize)
#define OPT_WRITE_SSIZE_T_MUTABLE(name, altname)                               \
	OPT_WRITE_MUTABLE(name, ssv, ssv2, sssz, emitter_type_ssize, altname)

#define OPT_WRITE_CHAR_P(name) OPT_WRITE(name, cpv, cpsz, emitter_type_string)

#endif /* JEMALLOC_INTERNAL_STATS_INTERNAL_H */
