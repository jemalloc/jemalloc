#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/ccache.h"

#include "jemalloc/internal/base.h"
#include "jemalloc/internal/arena_externs.h"
#include "jemalloc/internal/cache_bin.h"

#include <linux/rseq.h>

#define JEMALLOC_RSEQ_SIG 0x80088008

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

/* Beware of the init order, it must be assigned before ccache_init call */
extern unsigned ncpus;

/* Ccache is disabled by default */
bool opt_ccache = false;
size_t opt_ccache_max = CCACHE_MAXCLASS_LIMIT;

/*
 * Ccache handles sizes [ccache_minind; ccache_maxind).
 * If ccache is disabled, then ccache_minind = ccache_maxind = 0.
 */
szind_t ccache_minind = 0;
szind_t ccache_maxind = 0;
size_t ccache_maxclass = 0;

static size_t ccache_percpu_size;
static ccache_t *ccache_base;

static bool
ccache_handles_szind(szind_t ind) {
	return ccache_minind <= ind && ind < ccache_maxind;
}

static inline unsigned
ccache_nclasses() {
	assert(ccache_maxind >= ccache_minind);
	return ccache_maxind - ccache_minind;
}

static unsigned
ccache_bin_capacity() {
	return CCACHE_BIN_ELEMENTS;
}

static bool
ccache_bin_empty(ccache_bin_t *bin) {
	return bin->head == (void**)&bin->head;
}

static bool
ccache_bin_locked(ccache_bin_t *bin) {
	return bin->head == NULL;
}

static unsigned
ccache_bin_ncached_elements(ccache_bin_t *bin) {
	unsigned res = (unsigned)((uintptr_t)(&bin->head) -
	    (uintptr_t)(bin->head)) / sizeof(void*);
	assert(res <= ccache_bin_capacity());
	return res;
}

static ccache_t *
ccache_get(int cpu) {
	return (ccache_t*)((uintptr_t)ccache_base + ccache_percpu_size * cpu);
}

static ccache_bin_t *
ccache_bin_get(ccache_t *c, szind_t ind) {
	assert(ccache_handles_szind(ind));
	return &c->bins[ind - ccache_minind];
}

static void *
ccache_bin_finish_fill(ccache_bin_t *bin, cache_bin_ptr_array_t *arr,
    cache_bin_sz_t nfilled) {
	unsigned dest_index = arr->n - nfilled;
	if (nfilled < arr->n) {
		memmove(arr->ptr + dest_index, arr->ptr,
		    nfilled * sizeof(void *));
	}
	void *allocated = arr->ptr[dest_index];
	__asm__ __volatile__ ("" ::: "memory" );
	bin->head = &arr->ptr[dest_index + 1];
	return allocated;
}

/* Refills half of the bin */
static void *
ccache_bin_refill(tsd_t *tsd, ccache_t *ccache, ccache_bin_t *bin,
    arena_t *arena, szind_t binind, bool small) {
	const unsigned nfill = CCACHE_BIN_ELEMENTS / 2 + 1;
	assert(ccache_bin_locked(bin));
	assert(small);
	/*
	 * Here we can already be on a different CPU, i.e. rseq_abi->cpu might
	 * be different from 'cpu' argument. But there's no race, as the
	 * critical section assigns 0 to head, making all following allocations
	 * on the same CPU go down the slow path.
	 */

	atomic_fetch_add_u32(&ccache->stats.nfills, 1,
	    atomic_memory_order_relaxed);

	CACHE_BIN_PTR_ARRAY_DECLARE(ptrs, nfill);
	ptrs.ptr = &bin->ccache_bin_entry[CCACHE_BIN_ELEMENTS - nfill];

	unsigned nfilled = arena_fill_small(tsd_tsdn(tsd), arena, binind,
	    ptrs.ptr, nfill, NULL);
	return ccache_bin_finish_fill(bin, &ptrs, nfilled);
}

/* Flushes half of the bin */
static bool
ccache_bin_flush(tsd_t *tsd, ccache_t *ccache, ccache_bin_t *bin,
    arena_t *arena, szind_t binind, void *ptr, bool small) {
	const unsigned nflush = CCACHE_BIN_ELEMENTS / 2;
	assert(ccache_bin_locked(bin));

	atomic_fetch_add_u32(&ccache->stats.nflushes, 1,
	    atomic_memory_order_relaxed);

	CACHE_BIN_PTR_ARRAY_DECLARE(ptrs, nflush);
	/* 
	 * TODO: tcache is flushed differently - 'oldest' ptrs are flushed and
	 * the rest is memmoved to the beginning of the bin. It helps first fit,
	 * apparently. Here I pick the easiest way - flush the 'top' half
	 * (leftmost in the address space). Other policies should be tested as
	 * well.
	 */
	ptrs.ptr = bin->ccache_bin_entry;

	arena_flush(tsd, arena, &ptrs, binind, nflush, NULL, small,
	    /* ccache = */ true);

	bin->ccache_bin_entry[nflush - 1] = ptr;
	__asm__ __volatile__ ("" ::: "memory" );
	bin->head = &bin->ccache_bin_entry[nflush - 1];

	return false;
}

static void
ccache_bin_full_flush_unsafe(tsd_t *tsd, ccache_bin_t *bin, szind_t binind,
    bool small) {
	const unsigned nflush = ccache_bin_ncached_elements(bin);

	CACHE_BIN_PTR_ARRAY_DECLARE(ptrs, nflush);
	ptrs.ptr = &bin->ccache_bin_entry[CCACHE_BIN_ELEMENTS - nflush];

	arena_flush(tsd, *tsd_arenap_get(tsd), &ptrs, binind, nflush, NULL,
	    small, true);

	bin->head = (void *)&bin->head;
}

static inline cache_bin_stats_t *
ccache_bin_tstats_get(tsd_t *tsd, szind_t binind) {
	assert(binind >= ccache_minind);
	return &tsd_ccache_tdatap_get(tsd)->ccache_stats[binind - ccache_minind];
}

static void
ccache_bump_tstats(tsd_t *tsd, szind_t binind) {
	cache_bin_stats_t *stats = ccache_bin_tstats_get(tsd, binind);
	++stats->nrequests;
}

/* Depends on booted tcache */
bool
ccache_init(tsdn_t *tsdn, base_t *base) {
	assert(ncpus > 0);

	if (opt_ccache) {
		ccache_maxclass = sz_s2u(opt_ccache_max);

		ccache_minind = sz_size2index(tcache_maxclass) + 1;
		ccache_maxind = sz_size2index(ccache_maxclass) + 1;
	} else {
		ccache_maxclass = 0;
	}
	assert(ccache_nclasses() <= CCACHE_NBINS_LIMIT);

	ccache_percpu_size = sizeof(ccache_t) +
	    sizeof(ccache_bin_t) * ccache_nclasses();
	ccache_base = (ccache_t*)base_alloc(tsdn, base,
	    ccache_percpu_size * ncpus, PAGE);
	if (ccache_base == NULL) {
		return true;
	}
	for (unsigned i = 0; i < ncpus; ++i) {
		ccache_t *ith_cache = (ccache_t*)((uintptr_t)ccache_base +
		    ccache_percpu_size * i);
		for (unsigned j = 0; j < ccache_nclasses(); ++j) {
			ccache_bin_t *bin = &ith_cache->bins[j];
			bin->head = (void **)&bin->head;
		}
	}

	return false;
}

void*
ccache_alloc(tsd_t *tsd, arena_t *arena, size_t size, szind_t ind,
    bool zero, bool small) {
	assert(opt_ccache);
	assert(ccache_handles_szind(ind));
	/*
	 * Ccache is disabled for manual arenas due to difficulties handling
	 * arena reset.
	 */
	assert(arena_is_auto(arena));

	void *ret = NULL;
	volatile rseq_t *rseq_abi = &tsd_ccache_tdatap_get(tsd)->rseq_abi;
	ccache_t *ccache;
	ccache_bin_t *bin;
	/*
	 * No need to zero initialize these flags.
	 * If fallback, sete will set 'fallback_flag' to 1, 'refill_flag' is
	 * never read.
	 * If refill, sete will set 'fallback_flag' to 0, and setne will set
	 * 'refill_flag' to 1.
	 */
	bool fallback_flag, refill_flag;
#ifdef JEMALLOC_DEBUG
	int cpu;
	int spins = 0;
#endif

	size_t bin_offset = (ind - ccache_minind) * sizeof(ccache_bin_t) +
	    offsetof(ccache_t, bins);

	__asm__ __volatile__(
	    /* aw == Allocatable and writable section */
	    ".pushsection __rseq_table, \"aw\"\n\t"
	    ".balign 32\n\t"
	    "cs_obj_ccache_alloc:\n\t"
	    /* rseq_cs.version rseq_cs.flags */
	    ".long 0, 0\n\t"
	    /* rseq_cs.start_ip rseq_cs.post_commit_offset rseq_cs.abort_ip */
	    ".quad rseq_cs_start%=, rseq_cs_postcommit%= - rseq_cs_start%=, rseq_cs_abort%=\n\t"
	    ".popsection\n\t"

	    "restart%=:\n\t"
#ifdef JEMALLOC_DEBUG
	    "incl %[spins]\n\t"
#endif
	    /* Set the critical section pointer */
	    "leaq cs_obj_ccache_alloc(%%rip), %%rax\n\t"
	    "movq %%rax, %c[rseq_cs_offset](%[rseq_abi])\n\t"

	    /* -------------------- Begin ------------------- */
	    "rseq_cs_start%=:\n\t"
	    /* Get CPU id, ccache and bin pointers */
	    "movl %c[cpuid_offset](%[rseq_abi]), %%eax\n\t"
#ifdef JEMALLOC_DEBUG
	    "movl %%eax, %[cpu]\n\t"
#endif
	    "imulq %[ccache_percpu_size], %%rax\n\t"
	    "addq %[ccache_base], %%rax\n\t"
	    "movq %%rax, %[ccache]\n\t"
	    "leaq (%%rax, %[bin_offset]), %[bin]\n\t"

	    /* Read head value, decide between fallback, refill or alloc */
	    "movq %c[head_offset](%[bin]), %%rax\n\t"
	    /*
	     * If head == 0, another thread has put the bin into fallback mode.
	     * Set fallback flag and leave the critical section
	     */
	    "testq %%rax, %%rax\n\t"
	    "sete %[fallback_flag]\n\t"
	    "je rseq_cs_postcommit%=\n\t"

	    /* If the bin is empty, then put into fallback mode and refill */
	    "cmpq %%rax, (%%rax)\n\t"
	    "sete %[refill_flag]\n\t"
	    "je refill_needed%=\n\t"
	    /* Store allocation in ret and preapare head++ */
	    "movq (%%rax), %[ret]\n\t"
	    "addq $8, %%rax\n\t"
	    "jmp commit%=\n\t"

	    "refill_needed%=:\n\t"
	    /* Prepare head = 0 */
	    "xorl %%eax, %%eax\n\t"

	    /* Commit head++ or head = 0 and exit CS */
	    "commit%=:\n\t"
	    "movq %%rax, %c[head_offset](%[bin])\n\t"

	    /* ------------------- End ------------------- */
	    "rseq_cs_postcommit%=:\n\t"

	    /* ax == Allocatable and executable section */
	    ".pushsection __rseq_abort, \"ax\"\n\t"
	    ".byte 0x0f, 0x1f, 0x05\n\t"
	    ".long "STRINGIFY(JEMALLOC_RSEQ_SIG)"\n\t"
	    "rseq_cs_abort%=:\n\t"
	    /*
	     * Spin in case of abort - re-fetch per-cpu cache for the updated
	     * CPU value in rseq_abi and retry.
	     */
	    "jmp restart%=\n\t"
	    ".popsection\n\t"

	    : [ret] "=&r" (ret),
	      [fallback_flag] "=&r" (fallback_flag),
	      [refill_flag] "=&r" (refill_flag),
	      [bin] "=&r" (bin),
	      [ccache] "=&r" (ccache)
#ifdef JEMALLOC_DEBUG
	      , [cpu] "=m" (cpu),
	      [spins] "=m" (spins)
#endif
	    : [bin_offset] "r" (bin_offset),
	      [ccache_percpu_size] "r" (ccache_percpu_size),
	      [head_offset] "i" (offsetof(ccache_bin_t, head)),
	      [cpuid_offset] "i" (offsetof(rseq_t, cpu_id)),
	      [rseq_cs_offset] "i" (offsetof(rseq_t, rseq_cs)),
	      [rseq_abi] "r" (rseq_abi),
	      [ccache_base] "rm" (ccache_base)
	    : "rax", "cc", "memory"
	);
#ifdef JEMALLOC_DEBUG
	assert(spins > 0);
	assert(ccache_get(cpu) == ccache);
	assert(ccache_bin_get(ccache_get(cpu), ind) == bin);
#endif
	if (unlikely(fallback_flag)) {
		goto fallback;
	}
	if (unlikely(refill_flag)) {
		assert(bin->head == NULL);
		/*
		 * At this point, we're in the refill/flush mode. This thread is
		 * the owner of the refill, other threads willing to draw values
		 * from 'cpu' will jump to fallback label and return NULL.
		 */
		if (small) {
			ret =
			    ccache_bin_refill(tsd, ccache, bin, arena,
				ind, small);
		} else {
			/*
			 * Do not refill if large size class; transfer to the 
			 * empty state. Arena will handle zero and stats.
			 */
			bin->head = (void **)&bin->head;
fallback:
			return arena_malloc_hard(tsd_tsdn(tsd), arena, size,
			    ind, zero);
		}
	}
	assert(ret != NULL);
	if (unlikely(zero)) {
		size_t usize = sz_index2size(ind);
		assert(tcache_salloc(tsd_tsdn(tsd), ret) == usize);
		memset(ret, 0, usize);
	}
	ccache_bump_tstats(tsd, ind);

	return ret;
}

bool
ccache_free(tsd_t *tsd, void *ptr, szind_t ind, bool small) {
	assert(opt_ccache);
	assert(ccache_handles_szind(ind));

	edata_t *edata = emap_edata_lookup(tsd_tsdn(tsd),
	    &arena_emap_global, ptr);
	arena_t *origin_arena = arena_get_from_edata(edata);
	if (!arena_is_auto(origin_arena)) {
		return true;
	}
	volatile rseq_t *rseq_abi = &tsd_ccache_tdatap_get(tsd)->rseq_abi;
	ccache_t *ccache;
	ccache_bin_t *bin;
#ifdef JEMALLOC_DEBUG
	int cpu;
	int spins = 0;
#endif
	bool fallback_flag, flush_flag;

	size_t bin_offset = (ind - ccache_minind) * sizeof(ccache_bin_t) +
	    offsetof(ccache_t, bins);

	__asm__ __volatile__(
	    /* aw == Allocatable and writable section */
	    ".pushsection __rseq_table, \"aw\"\n\t"
	    ".balign 32\n\t"
	    "cs_obj_ccache_free:\n\t"
	    /* rseq_cs.version rseq_cs.flags */
	    ".long 0, 0\n\t"
	    /* rseq_cs.start_ip rseq_cs.post_commit_offset rseq_cs.abort_ip */
	    ".quad rseq_cs_start%=, rseq_cs_postcommit%= - rseq_cs_start%=, rseq_cs_abort%=\n\t"
	    ".popsection\n\t"

	    "restart%=:\n\t"
#ifdef JEMALLOC_DEBUG
	    "incl %[spins]\n\t"
#endif
	    /* Set the critical section pointer */
	    "leaq cs_obj_ccache_free(%%rip), %%rax\n\t"
	    "movq %%rax, %c[rseq_cs_offset](%[rseq_abi])\n\t"

	    /* -------------------- Begin ------------------- */
	    "rseq_cs_start%=:\n\t"
	    /* Get CPU id, ccache and bin pointers */
	    "movl %c[cpuid_offset](%[rseq_abi]), %%eax\n\t"
#ifdef JEMALLOC_DEBUG
	    "movl %%eax, %[cpu]\n\t"
#endif
	    "imulq %[ccache_percpu_size], %%rax\n\t"
	    "addq %[ccache_base], %%rax\n\t"
	    "movq %%rax, %[ccache]\n\t"
	    "leaq (%%rax, %[bin_offset]), %[bin]\n\t"

	    /* rax = head */
	    "movq %c[head_offset](%[bin]), %%rax\n\t"
	    /* Check if in fallback mode, then fallback */
	    "testq %%rax, %%rax\n\t"
	    "sete %[fallback_flag]\n\t"
	    "je rseq_cs_postcommit%=\n\t"
	    /* Check if the bin is full, then flush  */
	    "cmpq %%rax, %[bin]\n\t"
	    "sete %[flush_flag]\n\t"
	    "je flush_needed%=\n\t"
	    /* Prepare head-- */
	    "subq $8, %%rax\n\t"
	    "movq %[ptr], (%%rax)\n\t"
	    "jmp commit%=\n\t"

	    "flush_needed%=:\n\t"
	    /* Prepare head = 0 */
	    "xorl %%eax, %%eax\n\t"
	    /* Commit either head-- or head = 0 */
	    "commit%=:\n\t"
	    "movq %%rax, %c[head_offset](%[bin])\n\t"

	    /* ------------------- End ------------------- */
	    "rseq_cs_postcommit%=:\n\t"

	    /* ax == Allocatable and executable section */
	    ".pushsection __rseq_abort, \"ax\"\n\t"
	    ".byte 0x0f, 0x1f, 0x05\n\t"
	    ".long "STRINGIFY(JEMALLOC_RSEQ_SIG)"\n\t"
	    "rseq_cs_abort%=:\n\t"
	    "jmp restart%=\n\t"
	    ".popsection\n\t"
	    : [fallback_flag] "=&r" (fallback_flag),
	      [flush_flag] "=&r" (flush_flag),
	      [bin] "=&r" (bin),
	      [ccache] "=&r" (ccache)
#ifdef JEMALLOC_DEBUG
	      , [cpu] "=m" (cpu),
	      [spins] "=m" (spins)
#endif
	    : [bin_offset] "r" (bin_offset),
	      [ccache_percpu_size] "r" (ccache_percpu_size),
	      [head_offset] "i" (offsetof(ccache_bin_t, head)),
	      [cpuid_offset] "i" (offsetof(rseq_t, cpu_id)),
	      [rseq_cs_offset] "i" (offsetof(rseq_t, rseq_cs)),
	      [rseq_abi] "r" (rseq_abi),
	      [ccache_base] "rm" (ccache_base),
	      [ptr] "r" (ptr)
	    : "rax", "cc", "memory"
	 );
#ifdef JEMALLOC_DEBUG
	assert(spins > 0);
	assert(ccache_get(cpu) == ccache);
	assert(ccache_bin_get(ccache_get(cpu), ind) == bin);
#endif
	if (unlikely(fallback_flag)) {
		return true;
	}
	if (unlikely(flush_flag)) {
		assert(bin->head == NULL);
		return ccache_bin_flush(tsd, ccache, bin, tsd_arena_get(tsd),
		    ind, ptr, small);
	}
	return false;
}

uint32_t
ccache_nflushes_get() {
	assert(ncpus > 0);

	uint32_t res = 0;
	for (unsigned cpu = 0; cpu < ncpus; ++cpu) {
		ccache_t *ccache = ccache_get(cpu);
		res += atomic_load_u32(&ccache->stats.nflushes,
		    atomic_memory_order_relaxed);
	}
	return res;
}

uint32_t
ccache_nfills_get() {
	assert(ncpus > 0);

	uint32_t res = 0;
	for (unsigned cpu = 0; cpu < ncpus; ++cpu) {
		ccache_t *ccache = ccache_get(cpu);
		res += atomic_load_u32(&ccache->stats.nfills,
		    atomic_memory_order_relaxed);
	}
	return res;
}

typedef void (* ccache_bin_visitor)(ccache_bin_t *bin, int cpu, szind_t ind,
    void *ctx);

void
ccache_visit_bins(ccache_bin_visitor visitor, void *ctx) {
	assert(ncpus > 0);

	for (unsigned cpu = 0; cpu < ncpus; ++cpu) {
		ccache_t *ccache = ccache_get(cpu);
		for (szind_t ind = ccache_minind; ind < ccache_maxind; ++ind) {
			ccache_bin_t *bin = ccache_bin_get(ccache, ind);
			visitor(bin, cpu, ind, ctx);
		}
	}
}

static void
ccache_bin_flush_visitor(ccache_bin_t *bin, int cpu, szind_t ind, void *aux) {
	ccache_bin_full_flush_unsafe(tsd_fetch(), bin, ind, ind < SC_NBINS);
}

void
ccache_full_flush_unsafe() {
	ccache_visit_bins(ccache_bin_flush_visitor, NULL);
}

static void
ccache_bin_is_empty_visitor(ccache_bin_t *bin, int cpu, szind_t ind,
    void *res) {
	if (!ccache_bin_empty(bin)) {
		*((bool *)res) = false;
	}
}

bool
ccache_is_empty_unsafe() {
	bool is_empty = true;
	ccache_visit_bins(ccache_bin_is_empty_visitor, &is_empty);
	return is_empty;
}

static void
ccache_ncached_visitor(ccache_bin_t *bin, int cpu, szind_t ind, void *res) {
	*((int *)res) += ccache_bin_ncached_elements(bin);
}

int
ccache_ncached_elements_unsafe() {
	int result = 0;
	ccache_visit_bins(ccache_ncached_visitor, &result);
	return result;
}

void
tsd_ccache_init(tsd_t *tsd) {
	rseq_t *rseq_abi = &tsd_ccache_tdatap_get(tsd)->rseq_abi;
	long rc = syscall(__NR_rseq, rseq_abi, sizeof(rseq_t), 0,
	    JEMALLOC_RSEQ_SIG);
	if (rc) {
		/* TODO: handle failure here */
	}
}

void
ccache_merge_tstats(tsdn_t *tsdn) {
	cassert(config_stats);

	arena_t *arena = tsd_arena_get(tsdn_tsd(tsdn));
	if (arena == NULL) {
		return;
	}
	for (szind_t binind = ccache_minind; binind < ccache_maxind; ++binind) {
		cache_bin_stats_t *stats = ccache_bin_tstats_get(tsdn_tsd(tsdn), binind);
		assert(stats);

		if (stats->nrequests == 0) {
			continue;
		}

		/* TODO: remove duplication with tcache_stats_merge */
		if (binind < SC_NBINS) {
			bin_t *bin = arena_bin_choose(tsdn, arena, binind, NULL);
			malloc_mutex_lock(tsdn, &bin->lock);
			bin->stats.nrequests += stats->nrequests;
			malloc_mutex_unlock(tsdn, &bin->lock);
		} else {
			arena_stats_large_nrequests_add(tsdn,
			    &arena->stats, binind, stats->nrequests);
		}
		stats->nrequests = 0;
	}
}
