#ifndef JEMALLOC_INTERNAL_SEC_H
#define JEMALLOC_INTERNAL_SEC_H

#include "jemalloc/internal/atomic.h"
#include "jemalloc/internal/pai.h"

/*
 * Small extent cache.
 *
 * This includes some utilities to cache small extents.  We have a per-pszind
 * bin with its own lock and edata heap (including only extents of that size).
 * We don't try to do any coalescing of extents (since it would require
 * cross-bin locks).  As a result, we need to be careful about fragmentation.
 * As a gesture in that direction, we limit the size of caches, apply first-fit
 * within the bins, and, when flushing a bin, flush all of its extents rather
 * than just those up to some threshold.  When we allocate again, we'll get a
 * chance to move to better ones.
 */

/*
 * This is a *small* extent cache, after all.  Assuming 4k pages and an ngroup
 * of 4, this allows caching of sizes up to 128k.
 */
#define SEC_NPSIZES 16
/*
 * For now, we put a cap on the number of SECs an arena can have.  There's no
 * reason it can't be dynamic; it's just inconvenient.  This number of shards
 * are embedded in the arenas, so there's a space / configurability tradeoff
 * here.  Eventually, we should probably dynamically allocate only however many
 * we require.
 */
#define SEC_NSHARDS_MAX 8

/*
 * For now, this is just one field; eventually, we'll probably want to get more
 * fine-grained data out (like per-size class statistics).
 */
typedef struct sec_stats_s sec_stats_t;
struct sec_stats_s {
	/* Sum of bytes_cur across all shards. */
	size_t bytes;
};

static inline void
sec_stats_accum(sec_stats_t *dst, sec_stats_t *src) {
	dst->bytes += src->bytes;
}

typedef struct sec_shard_s sec_shard_t;
struct sec_shard_s {
	/*
	 * We don't keep per-bin mutexes, even though that would allow more
	 * sharding; this allows global cache-eviction, which in turn allows for
	 * better balancing across free lists.
	 */
	malloc_mutex_t mtx;
	/*
	 * A SEC may need to be shut down (i.e. flushed of its contents and
	 * prevented from further caching).  To avoid tricky synchronization
	 * issues, we just track enabled-status in each shard, guarded by a
	 * mutex.  In practice, this is only ever checked during brief races,
	 * since the arena-level atomic boolean tracking HPA enabled-ness means
	 * that we won't go down these pathways very often after custom extent
	 * hooks are installed.
	 */
	bool enabled;
	edata_list_active_t freelist[SEC_NPSIZES];
	size_t bytes_cur;
};

typedef struct sec_s sec_t;
struct sec_s {
	pai_t pai;
	pai_t *fallback;

	/*
	 * We'll automatically refuse to cache any objects in this sec if
	 * they're larger than alloc_max bytes.
	 */
	size_t alloc_max;
	/*
	 * Exceeding this amount of cached extents in a shard causes *all* of
	 * the shards in that bin to be flushed.
	 */
	size_t bytes_max;

	/*
	 * We don't necessarily always use all the shards; requests are
	 * distributed across shards [0, nshards - 1).
	 */
	size_t nshards;
	sec_shard_t shards[SEC_NSHARDS_MAX];
};

bool sec_init(sec_t *sec, pai_t *fallback, size_t nshards, size_t alloc_max,
    size_t bytes_max);
void sec_flush(tsdn_t *tsdn, sec_t *sec);
void sec_disable(tsdn_t *tsdn, sec_t *sec);

/*
 * Morally, these two stats methods probably ought to be a single one (and the
 * mutex_prof_data ought to live in the sec_stats_t.  But splitting them apart
 * lets them fit easily into the pa_shard stats framework (which also has this
 * split), which simplifies the stats management.
 */
void sec_stats_merge(tsdn_t *tsdn, sec_t *sec, sec_stats_t *stats);
void sec_mutex_stats_read(tsdn_t *tsdn, sec_t *sec,
    mutex_prof_data_t *mutex_prof_data);

/*
 * We use the arena lock ordering; these are acquired in phase 2 of forking, but
 * should be acquired before the underlying allocator mutexes.
 */
void sec_prefork2(tsdn_t *tsdn, sec_t *sec);
void sec_postfork_parent(tsdn_t *tsdn, sec_t *sec);
void sec_postfork_child(tsdn_t *tsdn, sec_t *sec);

#endif /* JEMALLOC_INTERNAL_SEC_H */
