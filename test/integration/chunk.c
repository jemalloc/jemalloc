#include "test/jemalloc_test.h"

chunk_alloc_t *old_alloc;
chunk_dealloc_t *old_dealloc;

bool
chunk_dealloc(void *chunk, size_t size, unsigned arena_ind)
{

	return (old_dealloc(chunk, size, arena_ind));
}

void *
chunk_alloc(size_t size, size_t alignment, bool *zero, unsigned arena_ind)
{

	return (old_alloc(size, alignment, zero, arena_ind));
}

TEST_BEGIN(test_chunk)
{
	void *p;
	chunk_alloc_t *new_alloc;
	chunk_dealloc_t *new_dealloc;
	size_t old_size, new_size;

	new_alloc = chunk_alloc;
	new_dealloc = chunk_dealloc;
	old_size = sizeof(chunk_alloc_t *);
	new_size = sizeof(chunk_alloc_t *);

	assert_d_eq(mallctl("arena.0.chunk.alloc", &old_alloc,
	    &old_size, &new_alloc, new_size), 0,
	    "Unexpected alloc error");
	assert_ptr_ne(old_alloc, new_alloc,
	    "Unexpected alloc error");
	assert_d_eq(mallctl("arena.0.chunk.dealloc", &old_dealloc,
	    &old_size, &new_dealloc, new_size), 0,
	    "Unexpected dealloc error");
	assert_ptr_ne(old_dealloc, new_dealloc,
	    "Unexpected dealloc error");

	p = mallocx(42, 0);
	assert_ptr_ne(p, NULL, "Unexpected alloc error");
	free(p);

	assert_d_eq(mallctl("arena.0.chunk.alloc", NULL,
	    NULL, &old_alloc, old_size), 0,
	    "Unexpected alloc error");
	assert_d_eq(mallctl("arena.0.chunk.dealloc", NULL,
	    NULL, &old_dealloc, old_size), 0,
	    "Unexpected dealloc error");
}
TEST_END

int
main(void)
{

	return (test(test_chunk));
}
