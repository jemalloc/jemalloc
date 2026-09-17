#include "test/jemalloc_test.h"

TEST_BEGIN(test_a0) {
	void *p;

	p = a0malloc(1);
	expect_ptr_not_null(p, "Unexpected a0malloc() error");
	a0dalloc(p);
}
TEST_END

TEST_BEGIN(test_a0_multi_sizes) {
	static const size_t sizes[] = {1, 8, 1024, 64 * 1024};
	void *ptrs[ARRAY_SIZE(sizes)];

	for (unsigned i = 0; i < ARRAY_SIZE(sizes); i++) {
		ptrs[i] = a0malloc(sizes[i]);
		expect_ptr_not_null(
		    ptrs[i], "Unexpected a0malloc(%zu) failure", sizes[i]);
		for (unsigned j = 0; j < i; j++) {
			expect_ptr_ne(ptrs[i], ptrs[j],
			    "a0malloc returned duplicate pointer (i=%u, j=%u)",
			    i, j);
		}
	}
	for (unsigned i = 0; i < ARRAY_SIZE(sizes); i++) {
		a0dalloc(ptrs[i]);
	}
}
TEST_END

TEST_BEGIN(test_a0_ialloc_zero) {
	static const size_t sizes[] = {1, 8, 1024, 64 * 1024};

	for (unsigned i = 0; i < ARRAY_SIZE(sizes); i++) {
		size_t   size = sizes[i];
		uint8_t *p = (uint8_t *)a0ialloc(size, /* zero */ true,
		    /* is_internal */ true);
		expect_ptr_not_null(p, "Unexpected a0ialloc(zero=true, %zu)",
		    size);
		for (size_t k = 0; k < size; k++) {
			expect_u_eq((unsigned)p[k], 0,
			    "a0ialloc(zero=true) byte %zu of %zu not zero",
			    k, size);
		}
		a0idalloc(p, /* is_internal */ true);
	}

	/* zero=false: just must not crash and must return non-NULL. */
	void *q = a0ialloc(64, /* zero */ false, /* is_internal */ true);
	expect_ptr_not_null(q, "Unexpected a0ialloc(zero=false) failure");
	a0idalloc(q, /* is_internal */ true);
}
TEST_END

TEST_BEGIN(test_a0_ialloc_internal_flag) {
	void *p_internal = a0ialloc(64, false, /* is_internal */ true);
	expect_ptr_not_null(p_internal, "a0ialloc(is_internal=true) failed");
	a0idalloc(p_internal, /* is_internal */ true);

	void *p_external = a0ialloc(64, false, /* is_internal */ false);
	expect_ptr_not_null(p_external, "a0ialloc(is_internal=false) failed");
	a0idalloc(p_external, /* is_internal */ false);
}
TEST_END

int
main(void) {
	return test_no_malloc_init(test_a0, test_a0_multi_sizes,
	    test_a0_ialloc_zero, test_a0_ialloc_internal_flag);
}
