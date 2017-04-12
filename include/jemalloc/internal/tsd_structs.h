#ifndef JEMALLOC_INTERNAL_TSD_STRUCTS_H
#define JEMALLOC_INTERNAL_TSD_STRUCTS_H

#include "jemalloc/internal/ql.h"

#if (!defined(JEMALLOC_MALLOC_THREAD_CLEANUP) && !defined(JEMALLOC_TLS) && \
    !defined(_WIN32))
struct tsd_init_block_s {
	ql_elm(tsd_init_block_t)	link;
	pthread_t			thread;
	void				*data;
};
struct tsd_init_head_s {
	ql_head(tsd_init_block_t)	blocks;
	malloc_mutex_t			lock;
};
#endif

/*
 * Thread-Specific-Data layout
 * --- data accessed on tcache fast path: state, rtree_ctx, stats, prof ---
 * s: state
 * e: tcache_enabled
 * m: thread_allocated (config_stats)
 * f: thread_deallocated (config_stats)
 * p: prof_tdata (config_prof)
 * c: rtree_ctx (rtree cache accessed on deallocation)
 * t: tcache
 * --- data not accessed on tcache fast path: arena related fields ---
 * d: arenas_tdata_bypass
 * r: reentrancy_level
 * x: narenas_tdata
 * i: iarena
 * a: arena
 * o: arenas_tdata
 * Loading TSD data is on the critical path of basically all malloc operations.
 * In particular, tcache and rtree_ctx rely on hot CPU cache to be effective.
 * Use a compact layout to reduce cache footprint.
 * +--- 64-bit and 64B cacheline; 1B each letter; First byte on the left. ---+
 * |----------------------------  1st cacheline  ----------------------------|
 * | sedrxxxx mmmmmmmm ffffffff pppppppp [c * 32  ........ ........ .......] |
 * |----------------------------  2nd cacheline  ----------------------------|
 * | [c * 64  ........ ........ ........ ........ ........ ........ .......] |
 * |----------------------------  3nd cacheline  ----------------------------|
 * | [c * 32  ........ ........ .......] iiiiiiii aaaaaaaa oooooooo [t...... |
 * +-------------------------------------------------------------------------+
 * Note: the entire tcache is embedded into TSD and spans multiple cachelines.
 *
 * The last 3 members (i, a and o) before tcache isn't really needed on tcache
 * fast path.  However we have a number of unused tcache bins and witnesses
 * (never touched unless config_debug) at the end of tcache, so we place them
 * there to avoid breaking the cachelines and possibly paging in an extra page.
 */
#define MALLOC_TSD							\
/*  O(name,			type,		[gs]et,	init,	cleanup) */ \
    O(tcache_enabled,		bool,		yes,	yes,	no)	\
    O(arenas_tdata_bypass,	bool,		no,	no,	no)	\
    O(reentrancy_level,		int8_t,		no,	no,	no)	\
    O(narenas_tdata,		uint32_t,	yes,	no,	no)	\
    O(thread_allocated,		uint64_t,	yes,	no,	no)	\
    O(thread_deallocated,	uint64_t,	yes,	no,	no)	\
    O(prof_tdata,		prof_tdata_t *,	yes,	no,	yes)	\
    O(rtree_ctx,		rtree_ctx_t,	no,	yes,	no)	\
    O(iarena,			arena_t *,	yes,	no,	yes)	\
    O(arena,			arena_t *,	yes,	no,	yes)	\
    O(arenas_tdata,		arena_tdata_t *,yes,	no,	yes)	\
    O(tcache,			tcache_t,	no,	no,	yes)	\
    O(witnesses,		witness_list_t,	no,	no,	yes)	\
    O(rtree_leaf_elm_witnesses,	rtree_leaf_elm_witness_tsd_t,		\
						no,	no,	no)	\
    O(witness_fork,		bool,		yes,	no,	no)

#define TSD_INITIALIZER {						\
    tsd_state_uninitialized,						\
    TCACHE_ENABLED_ZERO_INITIALIZER,					\
    false,								\
    0,									\
    0,									\
    0,									\
    0,									\
    NULL,								\
    RTREE_CTX_ZERO_INITIALIZER,						\
    NULL,								\
    NULL,								\
    NULL,								\
    TCACHE_ZERO_INITIALIZER,						\
    ql_head_initializer(witnesses),					\
    RTREE_ELM_WITNESS_TSD_INITIALIZER,					\
    false								\
}

struct tsd_s {
	tsd_state_t	state;
#define O(n, t, gs, i, c)						\
	t		n;
MALLOC_TSD
#undef O
};

/*
 * Wrapper around tsd_t that makes it possible to avoid implicit conversion
 * between tsd_t and tsdn_t, where tsdn_t is "nullable" and has to be
 * explicitly converted to tsd_t, which is non-nullable.
 */
struct tsdn_s {
	tsd_t	tsd;
};

static const tsd_t tsd_initializer = TSD_INITIALIZER;
UNUSED static const void *malloc_tsd_no_cleanup = (void *)0;

malloc_tsd_types(, tsd_t)

#endif /* JEMALLOC_INTERNAL_TSD_STRUCTS_H */
