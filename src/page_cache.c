#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/page_cache.h"

#define PAGE_CACHE_DEFAULT_TICKS 512

static void
page_cache_defragment_locked(tsdn_t *tsdn,
                             page_cache_t *page_cache, uint64_t idx) {
	page_cache->ticker[idx] = PAGE_CACHE_DEFAULT_TICKS;

	if (page_cache->water[idx] < 0) {
		page_cache->sz[idx] *= 2;
		if (page_cache->sz[idx] > 100) {
			page_cache->sz[idx] = 100;
		}
	} else if (page_cache->water[idx] > 0) {
		if (page_cache->sz[idx] > 1) {
			page_cache->sz[idx] /= 2;
		}
	}
	page_cache->water[idx] = page_cache->pages[idx];
}

void page_cache_postfork_child(tsdn_t *tsdn, page_cache_t *page_cache) {
	for (int i = 0; i < PAGE_CACHE_PAGES; i++) {
		malloc_mutex_postfork_child(tsdn, &page_cache->mutexes[i]);
	}
}

void page_cache_postfork_parent(tsdn_t *tsdn, page_cache_t *page_cache) {
	for (int i = 0; i < PAGE_CACHE_PAGES; i++) {
		malloc_mutex_postfork_parent(tsdn, &page_cache->mutexes[i]);
	}
}

void page_cache_prefork(tsdn_t *tsdn, page_cache_t *page_cache) {
	for (int i = 0; i < PAGE_CACHE_PAGES; i++) {
		malloc_mutex_prefork(tsdn, &page_cache->mutexes[i]);
	}
}

int page_cache_init(page_cache_t *page_cache) {
	for (int i = 0; i < PAGE_CACHE_PAGES; i++) {
		if (malloc_mutex_init(&page_cache->mutexes[i], "page_cache",
			WITNESS_RANK_PAGE_CACHE, malloc_mutex_rank_exclusive)) {
			return -1;
		}
		page_cache->pages[i] = 0;
		page_cache->sz[i] = 1;
		page_cache->water[i] = 0;
		page_cache->ticker[i] = PAGE_CACHE_DEFAULT_TICKS;
	}
	return 0;
}

bool page_cache_put(tsdn_t *tsdn, page_cache_t *page_cache, extent_t *slab) {
	uint64_t pages = extent_size_get(slab) >> LG_PAGE;
	int idx = page_cache_pages_to_idx(pages);
	if (idx < 0)
		return false;

	bool done = false;

	malloc_mutex_lock(tsdn, &page_cache->mutexes[idx]);
	uint64_t ind = page_cache->pages[idx];
	if (ind < PAGE_CACHE_SIZE && ind < page_cache->sz[idx]) {
		page_cache->cache[idx][page_cache->pages[idx]++] = slab;
		done = true;
	}
	if (--page_cache->ticker[idx] == 0) {
		page_cache_defragment_locked(tsdn, page_cache, idx);
	}
	malloc_mutex_unlock(tsdn, &page_cache->mutexes[idx]);

	return done;
}

extent_t* page_cache_get(tsdn_t *tsdn, page_cache_t *page_cache, uint64_t pages) {
	extent_t *slab = NULL;
	int idx = page_cache_pages_to_idx(pages);
	if (idx < 0)
		return NULL;

	malloc_mutex_lock(tsdn, &page_cache->mutexes[idx]);
	uint64_t ind = page_cache->pages[idx];
	if (ind > 0) {
		slab = page_cache->cache[idx][--page_cache->pages[idx]];
		if ((unsigned)ind - 1 < page_cache->water[idx]) {
			page_cache->water[idx] = ind - 1;
		} else {
			page_cache->water[idx] = -1;
		}
	}
	if (--page_cache->ticker[idx] == 0) {
		page_cache_defragment_locked(tsdn, page_cache, idx);
	}
	malloc_mutex_unlock(tsdn, &page_cache->mutexes[idx]);

	return slab;
}
