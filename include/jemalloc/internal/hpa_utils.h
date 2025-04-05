#ifndef JEMALLOC_INTERNAL_HPA_UTILS_H
#define JEMALLOC_INTERNAL_HPA_UTILS_H

#include "jemalloc/internal/hpa.h"

#define HPA_MIN_VAR_VEC_SIZE 8
#ifdef JEMALLOC_HAVE_PROCESS_MADVISE
typedef struct iovec hpa_io_vector_t;
#else
typedef struct {
    void *iov_base;
    size_t iov_len;
} hpa_io_vector_t;
#endif

/* Actually invoke hooks. If we fail vectorized, use single purges */
static void
hpa_try_vectorized_purge(
  hpa_shard_t *shard, hpa_io_vector_t *vec, size_t vlen, size_t nbytes) {
    bool success = opt_process_madvise_max_batch > 0
      && !shard->central->hooks.vectorized_purge(vec, vlen, nbytes);
    if (!success) {
        /* On failure, it is safe to purge again (potential perf
         * penalty) If kernel can tell exactly which regions
         * failed, we could avoid that penalty.
         */
        for (size_t i = 0; i < vlen; ++i) {
            shard->central->hooks.purge(vec[i].iov_base, vec[i].iov_len);
        }
    }
}

/*
 * This struct accumulates the regions for process_madvise.
 * It invokes the hook when batch limit is reached
 */
typedef struct {
    hpa_io_vector_t *vp;
    size_t cur;
    size_t total_bytes;
    size_t capacity;
} hpa_range_accum_t;

static inline void
hpa_range_accum_init(hpa_range_accum_t *ra, hpa_io_vector_t *v, size_t sz) {
    ra->vp = v;
    ra->capacity = sz;
    ra->total_bytes = 0;
    ra->cur = 0;
}

static inline void
hpa_range_accum_flush(hpa_range_accum_t *ra, hpa_shard_t *shard) {
    assert(ra->total_bytes > 0 && ra->cur > 0);
    hpa_try_vectorized_purge(shard, ra->vp, ra->cur, ra->total_bytes);
    ra->cur = 0;
    ra->total_bytes = 0;
}

static inline void
hpa_range_accum_add(
  hpa_range_accum_t *ra, void *addr, size_t sz, hpa_shard_t *shard) {
    assert(ra->cur < ra->capacity);

    ra->vp[ra->cur].iov_base = addr;
    ra->vp[ra->cur].iov_len = sz;
    ra->total_bytes += sz;
    ra->cur++;

    if (ra->cur == ra->capacity) {
        hpa_range_accum_flush(ra, shard);
    }
}

static inline void
hpa_range_accum_finish(hpa_range_accum_t *ra, hpa_shard_t *shard) {
    if (ra->cur > 0) {
        hpa_range_accum_flush(ra, shard);
    }
}

/*
 * For purging more than one page we use batch of these items
 */
typedef struct {
	hpdata_purge_state_t state;
	hpdata_t *hp;
	bool dehugify;
} hpa_purge_item_t;

typedef struct hpa_purge_batch_s hpa_purge_batch_t;
struct hpa_purge_batch_s {
	hpa_purge_item_t *items;
	size_t items_capacity;
	/* Number of huge pages to purge in current batch */
	size_t item_cnt;
	/* Number of ranges to purge in current batch */
	size_t nranges;
	/* Total number of dirty pages in current batch*/
	size_t ndirty_in_batch;

	/* Max number of huge pages to purge */
	size_t max_hp;
	/*
	 * Once we are above this watermark we should not add more pages
	 * to the same batch. This is because while we want to minimize
	 * number of madvise calls we also do not want to be preventing
	 * allocations from too many huge pages (which we have to do
	 * while they are being purged)
	 */
	size_t range_watermark;

	size_t npurged_hp_total;
};

#endif /* JEMALLOC_INTERNAL_HPA_UTILS_H */
