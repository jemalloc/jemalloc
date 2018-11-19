#ifndef JEMALLOC_INTERNAL_PAGE_CACHE_H
#define JEMALLOC_INTERNAL_PAGE_CACHE_H

#define PAGE_CACHE_PAGES 7
#define PAGE_CACHE_SIZE 100

JEMALLOC_ALWAYS_INLINE int
page_cache_pages_to_idx(uint64_t pages) {
	if (pages >= 1 && pages <= PAGE_CACHE_PAGES)
		return (int)(pages - 1);
	return -1;
}

struct page_cache_s {
	malloc_mutex_t mutexes[PAGE_CACHE_PAGES];
	void* cache[PAGE_CACHE_PAGES][PAGE_CACHE_SIZE];
	uint64_t pages[PAGE_CACHE_PAGES];
	uint64_t sz[PAGE_CACHE_PAGES];
	int64_t water[PAGE_CACHE_PAGES];
	uint64_t ticker[PAGE_CACHE_PAGES];
};

typedef struct page_cache_s page_cache_t;

bool page_cache_put(tsdn_t *tsdn, page_cache_t *page_cache, extent_t *slab);
extent_t* page_cache_get(tsdn_t *tsdn, page_cache_t *page_cache, uint64_t pages);

void page_cache_postfork_child(tsdn_t *tsdn, page_cache_t *page_cache);
void page_cache_postfork_parent(tsdn_t *tsdn, page_cache_t *page_cache);
void page_cache_prefork(tsdn_t *tsdn, page_cache_t *page_cache);
int page_cache_init(page_cache_t *page_cache);

#endif /* JEMALLOC_INTERNAL_PAGE_CACHE_H */
