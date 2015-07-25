#include "test/jemalloc_test.h"

chunk_alloc_t *old_alloc;
chunk_dalloc_t *old_dalloc;
chunk_purge_t *old_purge;
bool purged;

void *
chunk_alloc(void *new_addr, size_t size, size_t alignment, bool *zero,
    unsigned arena_ind)
{

	return (old_alloc(new_addr, size, alignment, zero, arena_ind));
}

bool
chunk_dalloc(void *chunk, size_t size, unsigned arena_ind)
{

	return (old_dalloc(chunk, size, arena_ind));
}

bool
chunk_purge(void *chunk, size_t offset, size_t length, unsigned arena_ind)
{

	purged = true;
	return (old_purge(chunk, offset, length, arena_ind));
}

TEST_BEGIN(test_chunk)
{
	void *p;
	chunk_alloc_t *new_alloc;
	chunk_dalloc_t *new_dalloc;
	chunk_purge_t *new_purge;
	size_t old_size, new_size, huge0, huge1, huge2, sz;

	new_alloc = chunk_alloc;
	new_dalloc = chunk_dalloc;
	new_purge = chunk_purge;
	old_size = sizeof(chunk_alloc_t *);
	new_size = sizeof(chunk_alloc_t *);

	assert_d_eq(mallctl("arena.0.chunk.alloc", &old_alloc, &old_size,
	    &new_alloc, new_size), 0, "Unexpected alloc error");
	assert_ptr_ne(old_alloc, new_alloc, "Unexpected alloc error");

	assert_d_eq(mallctl("arena.0.chunk.dalloc", &old_dalloc, &old_size,
	    &new_dalloc, new_size), 0, "Unexpected dalloc error");
	assert_ptr_ne(old_dalloc, new_dalloc, "Unexpected dalloc error");

	assert_d_eq(mallctl("arena.0.chunk.purge", &old_purge, &old_size,
	    &new_purge, new_size), 0, "Unexpected purge error");
	assert_ptr_ne(old_purge, new_purge, "Unexpected purge error");

	sz = sizeof(size_t);
	assert_d_eq(mallctl("arenas.hchunk.0.size", &huge0, &sz, NULL, 0), 0,
	    "Unexpected arenas.hchunk.0.size failure");
	assert_d_eq(mallctl("arenas.hchunk.1.size", &huge1, &sz, NULL, 0), 0,
	    "Unexpected arenas.hchunk.1.size failure");
	assert_d_eq(mallctl("arenas.hchunk.2.size", &huge2, &sz, NULL, 0), 0,
	    "Unexpected arenas.hchunk.2.size failure");
	if (huge0 * 2 > huge2) {
		/*
		 * There are at least four size classes per doubling, so a
		 * successful xallocx() from size=huge2 to size=huge1 is
		 * guaranteed to leave trailing purgeable memory.
		 */
		p = mallocx(huge2, 0);
		assert_ptr_not_null(p, "Unexpected mallocx() error");
		purged = false;
		assert_zu_eq(xallocx(p, huge1, 0, 0), huge1,
		    "Unexpected xallocx() failure");
		assert_true(purged, "Unexpected purge");
		dallocx(p, 0);
	}

	p = mallocx(42, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	free(p);

	assert_d_eq(mallctl("arena.0.chunk.alloc", NULL, NULL, &old_alloc,
	    old_size), 0, "Unexpected alloc error");
	assert_d_eq(mallctl("arena.0.chunk.dalloc", NULL, NULL, &old_dalloc,
	    old_size), 0, "Unexpected dalloc error");
	assert_d_eq(mallctl("arena.0.chunk.purge", NULL, NULL, &old_purge,
	    old_size), 0, "Unexpected purge error");
}
TEST_END

int
main(void)
{

	return (test(test_chunk));
}
