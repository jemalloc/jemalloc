#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/arena.h"
#include "jemalloc/internal/arena_inlines.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/bin.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/witness.h"

/*
 * SVE batch extraction relies on 64-bit bitmap groups; see
 * bin_slab_reg_extract_sve().
 */
#if defined(__ARM_FEATURE_SVE) && LG_SIZEOF_BITMAP == 3
#	include <arm_sve.h>
#	define BIN_SLAB_REG_EXTRACT_SVE
#endif

bool
bin_update_shard_size(unsigned bin_shard_sizes[SC_NBINS_MAX], size_t start_size,
    size_t end_size, size_t nshards) {
	if (nshards > BIN_SHARDS_MAX || nshards == 0) {
		return true;
	}

	if (start_size > SC_SMALL_MAXCLASS) {
		return false;
	}
	if (end_size > SC_SMALL_MAXCLASS) {
		end_size = SC_SMALL_MAXCLASS;
	}

	/* Compute the index since this may happen before sz init. */
	szind_t ind1 = sz_size2index_compute(start_size);
	szind_t ind2 = sz_size2index_compute(end_size);
	for (unsigned i = ind1; i <= ind2; i++) {
		bin_shard_sizes[i] = (unsigned)nshards;
	}

	return false;
}

void
bin_shard_sizes_boot(unsigned bin_shard_sizes[SC_NBINS_MAX]) {
	/* Load the default number of shards. */
	for (unsigned i = 0; i < SC_NBINS; i++) {
		bin_shard_sizes[i] = N_BIN_SHARDS_DEFAULT;
	}
}

bool
bin_init(bin_t *bin) {
	if (malloc_mutex_init(&bin->lock, "bin", WITNESS_RANK_BIN,
	        malloc_mutex_rank_exclusive)) {
		return true;
	}
	bin->slabcur = NULL;
	edata_heap_new(&bin->slabs_nonfull);
	edata_list_active_init(&bin->slabs_full);
	if (config_stats) {
		memset(&bin->stats, 0, sizeof(bin_stats_t));
	}
	return false;
}

void
bin_prefork(tsdn_t *tsdn, bin_t *bin) {
	malloc_mutex_prefork(tsdn, &bin->lock);
}

void
bin_postfork_parent(tsdn_t *tsdn, bin_t *bin) {
	malloc_mutex_postfork_parent(tsdn, &bin->lock);
}

void
bin_postfork_child(tsdn_t *tsdn, bin_t *bin) {
	malloc_mutex_postfork_child(tsdn, &bin->lock);
}

JET_EXTERN void *
bin_slab_reg_alloc(edata_t *slab, const bin_info_t *bin_info) {
	void        *ret;
	slab_data_t *slab_data = edata_slab_data_get(slab);
	size_t       regind;

	assert(edata_nfree_get(slab) > 0);
	assert(!bitmap_full(slab_data->bitmap, &bin_info->bitmap_info));

	regind = bitmap_sfu(slab_data->bitmap, &bin_info->bitmap_info);
	ret = (void *)((byte_t *)edata_addr_get(slab)
	    + (uintptr_t)(bin_info->reg_size * regind));
	edata_nfree_dec(slab);
	return ret;
}

#ifdef BIN_SLAB_REG_EXTRACT_SVE
/*
 * Store the addresses of up to nmax regions, taken in ascending order from the
 * set bits of *gp, to ptrs, clear the taken bits from *gp, and return the
 * number taken.
 *
 * Each 32-bit lane tests one bit of the group, svcompact packs the indices of
 * the set bits to the front of the vector, and the addresses are computed and
 * stored from the vector registers, so there is no loop-carried dependency
 * from one region to the next.  The group is scanned in chunks of at most 32
 * bits, so that the per-lane mask 1 << lane stays below the element width and
 * (uint32_t)(g >> chunk) holds exactly the bits being tested, for any vector
 * length (including 2048 bits, where svcntw() == 64).  A chunk yields at most
 * 32 indices, which fit in the two 64-bit halves of the vector.
 */
static inline size_t
bin_slab_reg_extract_sve(bitmap_t *gp, size_t shift, uintptr_t base,
    uintptr_t regsize, size_t nmax, void **ptrs) {
	bitmap_t       g = *gp;
	const uint32_t nw = (uint32_t)svcntw();
	const uint32_t stride = nw < 32 ? nw : 32;
	const uint64_t nd = svcntd();
	const svbool_t pg = svwhilelt_b32_u32(0, stride);
	const svuint32_t lane = svindex_u32(0, 1);
	const svuint32_t lane_bit = svlsl_u32_x(pg, svdup_n_u32(1), lane);
	const svuint64_t vbase = svdup_n_u64((uint64_t)base);
	size_t   n = 0;
	unsigned last = 0;

	assert(g != 0 && nmax > 0);
	for (unsigned chunk = 0; chunk < BITMAP_GROUP_NBITS && n < nmax;
	    chunk += stride) {
		svbool_t set = svcmpne_n_u32(pg,
		    svand_n_u32_x(pg, lane_bit, (uint32_t)(g >> chunk)), 0);
		uint64_t take = svcntp_b32(pg, set);
		if (take == 0) {
			continue;
		}
		if (take > nmax - n) {
			take = nmax - n;
		}
		svuint32_t pos = svcompact_u32(
		    set, svadd_n_u32_x(pg, lane, chunk));
		last = svlastb_u32(svwhilelt_b32_u64(0, take), pos);

		uint64_t nlo = take < nd ? take : nd;
		svbool_t pglo = svwhilelt_b64_u64(0, nlo);
		svuint64_t ind = svadd_n_u64_x(
		    pglo, svunpklo_u64(pos), (uint64_t)shift);
		svst1_u64(pglo, (uint64_t *)(ptrs + n),
		    svmla_n_u64_x(pglo, vbase, ind, (uint64_t)regsize));
		if (take > nd) {
			svbool_t pghi = svwhilelt_b64_u64(0, take - nd);
			ind = svadd_n_u64_x(
			    pghi, svunpkhi_u64(pos), (uint64_t)shift);
			svst1_u64(pghi, (uint64_t *)(ptrs + n + nd),
			    svmla_n_u64_x(pghi, vbase, ind, (uint64_t)regsize));
		}
		n += take;
	}
	assert(n > 0);
	/* Every set bit at or below the last one taken has been taken. */
	*gp = g & ((~(bitmap_t)0 << last) << 1);
	return n;
}
#endif

void
bin_slab_reg_alloc_batch(
    edata_t *slab, const bin_info_t *bin_info, unsigned cnt, void **ptrs) {
	slab_data_t *slab_data = edata_slab_data_get(slab);

	assert(edata_nfree_get(slab) >= cnt);
	assert(!bitmap_full(slab_data->bitmap, &bin_info->bitmap_info));

	/*
	 * Take as many regions as possible from each bottom-level group, so
	 * that the (possibly multi-level) search for a group with free regions
	 * and the propagation of group fullness are done once per group rather
	 * than once per region.
	 */
	uintptr_t base = (uintptr_t)edata_addr_get(slab);
	uintptr_t regsize = (uintptr_t)bin_info->reg_size;
	unsigned  i = 0;
	size_t    group = 0;
	while (i < cnt) {
		group = bitmap_ffu_group(
		    slab_data->bitmap, &bin_info->bitmap_info, group);
		bitmap_t g = slab_data->bitmap[group];
		size_t shift = group << LG_BITMAP_GROUP_NBITS;
#ifdef BIN_SLAB_REG_EXTRACT_SVE
		i += (unsigned)bin_slab_reg_extract_sve(
		    &g, shift, base, regsize, cnt - i, ptrs + i);
#else
#if (!defined JEMALLOC_INTERNAL_POPCOUNTL) || (defined BITMAP_USE_TREE)
		size_t nalloc = cnt - i;
#else
		size_t nalloc = popcount_lu(g);
		if (nalloc > (cnt - i)) {
			nalloc = cnt - i;
		}
#endif
		while (nalloc--) {
			size_t bit = cfs_lu(&g);
			size_t regind = shift + bit;
			/* NOLINTNEXTLINE(performance-no-int-to-ptr) */
			*(ptrs + i) = (void *)(base + regsize * regind);
			i++;
#if (!defined JEMALLOC_INTERNAL_POPCOUNTL) || (defined BITMAP_USE_TREE)
			if (g == 0) {
				break;
			}
#endif
		}
#endif
		bitmap_group_set(
		    slab_data->bitmap, &bin_info->bitmap_info, group, g);
		/*
		 * If the batch is incomplete, this group is exhausted.  The bin
		 * lock prevents earlier groups from becoming available again.
		 */
		assert(i == cnt || g == 0);
		group++;
	}
	edata_nfree_sub(slab, cnt);
}

JET_EXTERN void
bin_slabs_nonfull_insert(bin_t *bin, edata_t *slab) {
	assert(edata_nfree_get(slab) > 0);
	edata_heap_insert(&bin->slabs_nonfull, slab);
	if (config_stats) {
		bin->stats.nonfull_slabs++;
	}
}

JET_EXTERN void
bin_slabs_nonfull_remove(bin_t *bin, edata_t *slab) {
	edata_heap_remove(&bin->slabs_nonfull, slab);
	if (config_stats) {
		bin->stats.nonfull_slabs--;
	}
}

JET_EXTERN edata_t *
bin_slabs_nonfull_tryget(bin_t *bin) {
	edata_t *slab = edata_heap_remove_first(&bin->slabs_nonfull);
	if (slab == NULL) {
		return NULL;
	}
	if (config_stats) {
		bin->stats.reslabs++;
		bin->stats.nonfull_slabs--;
	}
	return slab;
}

JET_EXTERN void
bin_slabs_full_insert(bool is_auto, bin_t *bin, edata_t *slab) {
	assert(edata_nfree_get(slab) == 0);
	/*
	 *  Tracking extents is required by arena_reset, which is not allowed
	 *  for auto arenas.  Bypass this step to avoid touching the edata
	 *  linkage (often results in cache misses) for auto arenas.
	 */
	if (is_auto) {
		return;
	}
	edata_list_active_append(&bin->slabs_full, slab);
}

void
bin_slabs_full_remove(bool is_auto, bin_t *bin, edata_t *slab) {
	if (is_auto) {
		return;
	}
	edata_list_active_remove(&bin->slabs_full, slab);
}

JET_EXTERN void
bin_dissociate_slab(bool is_auto, edata_t *slab, bin_t *bin) {
	/* Dissociate slab from bin. */
	if (slab == bin->slabcur) {
		bin->slabcur = NULL;
	} else {
		szind_t           binind = edata_szind_get(slab);
		const bin_info_t *bin_info = &bin_infos[binind];

		/*
		 * The following block's conditional is necessary because if the
		 * slab only contains one region, then it never gets inserted
		 * into the non-full slabs heap.
		 */
		if (bin_info->nregs == 1) {
			bin_slabs_full_remove(is_auto, bin, slab);
		} else {
			bin_slabs_nonfull_remove(bin, slab);
		}
	}
}

JET_EXTERN void
bin_lower_slab(tsdn_t *tsdn, bool is_auto, edata_t *slab, bin_t *bin) {
	assert(edata_nfree_get(slab) > 0);

	/*
	 * Make sure that if bin->slabcur is non-NULL, it refers to the
	 * oldest/lowest non-full slab.  It is okay to NULL slabcur out rather
	 * than proactively keeping it pointing at the oldest/lowest non-full
	 * slab.
	 */
	if (bin->slabcur != NULL && edata_snad_comp(bin->slabcur, slab) > 0) {
		/* Switch slabcur. */
		if (edata_nfree_get(bin->slabcur) > 0) {
			bin_slabs_nonfull_insert(bin, bin->slabcur);
		} else {
			bin_slabs_full_insert(is_auto, bin, bin->slabcur);
		}
		bin->slabcur = slab;
		if (config_stats) {
			bin->stats.reslabs++;
		}
	} else {
		bin_slabs_nonfull_insert(bin, slab);
	}
}

JET_EXTERN void
bin_dalloc_slab_prepare(tsdn_t *tsdn, edata_t *slab, bin_t *bin) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);

	assert(slab != bin->slabcur);
	if (config_stats) {
		bin->stats.curslabs--;
	}
}

void
bin_dalloc_locked_handle_newly_empty(
    tsdn_t *tsdn, bool is_auto, edata_t *slab, bin_t *bin) {
	bin_dissociate_slab(is_auto, slab, bin);
	bin_dalloc_slab_prepare(tsdn, slab, bin);
}

void
bin_dalloc_locked_handle_newly_nonempty(
    tsdn_t *tsdn, bool is_auto, edata_t *slab, bin_t *bin) {
	bin_slabs_full_remove(is_auto, bin, slab);
	bin_lower_slab(tsdn, is_auto, slab, bin);
}

void
bin_refill_slabcur_with_fresh_slab(tsdn_t *tsdn, bin_t *bin,
    szind_t binind, edata_t *fresh_slab) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);
	/* Only called after slabcur and nonfull both failed. */
	assert(bin->slabcur == NULL);
	assert(edata_heap_first(&bin->slabs_nonfull) == NULL);
	assert(fresh_slab != NULL);

	/* A new slab from arena_slab_alloc() */
	assert(edata_nfree_get(fresh_slab) == bin_infos[binind].nregs);
	if (config_stats) {
		bin->stats.nslabs++;
		bin->stats.curslabs++;
	}
	bin->slabcur = fresh_slab;
}

void *
bin_malloc_with_fresh_slab(tsdn_t *tsdn, bin_t *bin,
    szind_t binind, edata_t *fresh_slab) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);
	bin_refill_slabcur_with_fresh_slab(tsdn, bin, binind, fresh_slab);

	return bin_slab_reg_alloc(bin->slabcur, &bin_infos[binind]);
}

bool
bin_refill_slabcur_no_fresh_slab(tsdn_t *tsdn, bool is_auto, bin_t *bin) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);
	/* Only called after bin_slab_reg_alloc[_batch] failed. */
	assert(bin->slabcur == NULL || edata_nfree_get(bin->slabcur) == 0);

	if (bin->slabcur != NULL) {
		bin_slabs_full_insert(is_auto, bin, bin->slabcur);
	}

	/* Look for a usable slab. */
	bin->slabcur = bin_slabs_nonfull_tryget(bin);
	assert(bin->slabcur == NULL || edata_nfree_get(bin->slabcur) > 0);

	return (bin->slabcur == NULL);
}

void *
bin_malloc_no_fresh_slab(tsdn_t *tsdn, bool is_auto, bin_t *bin,
    szind_t binind) {
	malloc_mutex_assert_owner(tsdn, &bin->lock);
	if (bin->slabcur == NULL || edata_nfree_get(bin->slabcur) == 0) {
		if (bin_refill_slabcur_no_fresh_slab(tsdn, is_auto, bin)) {
			return NULL;
		}
	}

	assert(bin->slabcur != NULL && edata_nfree_get(bin->slabcur) > 0);
	return bin_slab_reg_alloc(bin->slabcur, &bin_infos[binind]);
}

bin_t *
bin_choose(tsdn_t *tsdn, arena_t *arena, szind_t binind,
    unsigned *binshard_p) {
	unsigned binshard;
	if (tsdn_null(tsdn) || tsd_arena_get(tsdn_tsd(tsdn)) == NULL) {
		binshard = 0;
	} else {
		binshard = tsd_binshardsp_get(tsdn_tsd(tsdn))->binshard[binind];
	}
	assert(binshard < bin_infos[binind].n_shards);
	if (binshard_p != NULL) {
		*binshard_p = binshard;
	}
	return arena_get_bin(arena, binind, binshard);
}

void *
bin_current_slab_addr(tsdn_t *tsdn, bin_t *bin) {
	malloc_mutex_lock(tsdn, &bin->lock);
	edata_t *slab = (bin->slabcur != NULL)
	    ? bin->slabcur
	    : edata_heap_first(&bin->slabs_nonfull);
	assert(slab != NULL || edata_heap_empty(&bin->slabs_nonfull));
	void *ret = (slab != NULL) ? edata_addr_get(slab) : NULL;
	assert(ret != NULL || slab == NULL);
	malloc_mutex_unlock(tsdn, &bin->lock);
	return ret;
}
