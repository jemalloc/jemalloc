#include "test/jemalloc_test.h"

#define CHUNK 0x400000
/* #define MAXALIGN ((size_t)UINT64_C(0x80000000000)) */
#define MAXALIGN ((size_t)0x2000000LU)
#define NITER 4

TEST_BEGIN(test_basic)
{
	size_t nsz, rsz, sz;
	void *p;

	sz = 42;
	nsz = 0;
	assert_d_eq(nallocm(&nsz, sz, 0), ALLOCM_SUCCESS,
	    "Unexpected nallocm() error");
	rsz = 0;
	assert_d_eq(allocm(&p, &rsz, sz, 0), ALLOCM_SUCCESS,
	    "Unexpected allocm() error");
	assert_zu_ge(rsz, sz, "Real size smaller than expected");
	assert_zu_eq(nsz, rsz, "nallocm()/allocm() rsize mismatch");
	assert_d_eq(dallocm(p, 0), ALLOCM_SUCCESS,
	    "Unexpected dallocm() error");

	assert_d_eq(allocm(&p, NULL, sz, 0), ALLOCM_SUCCESS,
	    "Unexpected allocm() error");
	assert_d_eq(dallocm(p, 0), ALLOCM_SUCCESS,
	    "Unexpected dallocm() error");

	nsz = 0;
	assert_d_eq(nallocm(&nsz, sz, ALLOCM_ZERO), ALLOCM_SUCCESS,
	    "Unexpected nallocm() error");
	rsz = 0;
	assert_d_eq(allocm(&p, &rsz, sz, ALLOCM_ZERO), ALLOCM_SUCCESS,
	    "Unexpected allocm() error");
	assert_zu_eq(nsz, rsz, "nallocm()/allocm() rsize mismatch");
	assert_d_eq(dallocm(p, 0), ALLOCM_SUCCESS,
	    "Unexpected dallocm() error");
}
TEST_END

TEST_BEGIN(test_alignment_errors)
{
	void *p;
	size_t nsz, rsz, sz, alignment;

#if LG_SIZEOF_PTR == 3
	alignment = UINT64_C(0x8000000000000000);
	sz        = UINT64_C(0x8000000000000000);
#else
	alignment = 0x80000000LU;
	sz        = 0x80000000LU;
#endif
	nsz = 0;
	assert_d_ne(nallocm(&nsz, sz, ALLOCM_ALIGN(alignment)), ALLOCM_SUCCESS,
	    "Expected error for nallocm(&nsz, %zu, %#x)",
	    sz, ALLOCM_ALIGN(alignment));
	rsz = 0;
	assert_d_ne(allocm(&p, &rsz, sz, ALLOCM_ALIGN(alignment)),
	    ALLOCM_SUCCESS, "Expected error for allocm(&p, %zu, %#x)",
	    sz, ALLOCM_ALIGN(alignment));
	assert_zu_eq(nsz, rsz, "nallocm()/allocm() rsize mismatch");

#if LG_SIZEOF_PTR == 3
	alignment = UINT64_C(0x4000000000000000);
	sz        = UINT64_C(0x8400000000000001);
#else
	alignment = 0x40000000LU;
	sz        = 0x84000001LU;
#endif
	nsz = 0;
	assert_d_eq(nallocm(&nsz, sz, ALLOCM_ALIGN(alignment)), ALLOCM_SUCCESS,
	    "Unexpected nallocm() error");
	rsz = 0;
	assert_d_ne(allocm(&p, &rsz, sz, ALLOCM_ALIGN(alignment)),
	    ALLOCM_SUCCESS, "Expected error for allocm(&p, %zu, %#x)",
	    sz, ALLOCM_ALIGN(alignment));

	alignment = 0x10LU;
#if LG_SIZEOF_PTR == 3
	sz = UINT64_C(0xfffffffffffffff0);
#else
	sz = 0xfffffff0LU;
#endif
	nsz = 0;
	assert_d_ne(nallocm(&nsz, sz, ALLOCM_ALIGN(alignment)), ALLOCM_SUCCESS,
	    "Expected error for nallocm(&nsz, %zu, %#x)",
	    sz, ALLOCM_ALIGN(alignment));
	rsz = 0;
	assert_d_ne(allocm(&p, &rsz, sz, ALLOCM_ALIGN(alignment)),
	    ALLOCM_SUCCESS, "Expected error for allocm(&p, %zu, %#x)",
	    sz, ALLOCM_ALIGN(alignment));
	assert_zu_eq(nsz, rsz, "nallocm()/allocm() rsize mismatch");
}
TEST_END

TEST_BEGIN(test_alignment_and_size)
{
	int r;
	size_t nsz, rsz, sz, alignment, total;
	unsigned i;
	void *ps[NITER];

	for (i = 0; i < NITER; i++)
		ps[i] = NULL;

	for (alignment = 8;
	    alignment <= MAXALIGN;
	    alignment <<= 1) {
		total = 0;
		for (sz = 1;
		    sz < 3 * alignment && sz < (1U << 31);
		    sz += (alignment >> (LG_SIZEOF_PTR-1)) - 1) {
			for (i = 0; i < NITER; i++) {
				nsz = 0;
				r = nallocm(&nsz, sz, ALLOCM_ALIGN(alignment) |
				    ALLOCM_ZERO);
				assert_d_eq(r, ALLOCM_SUCCESS,
				    "nallocm() error for alignment=%zu, "
				    "size=%zu (%#zx): %d",
				    alignment, sz, sz, r);
				rsz = 0;
				r = allocm(&ps[i], &rsz, sz,
				    ALLOCM_ALIGN(alignment) | ALLOCM_ZERO);
				assert_d_eq(r, ALLOCM_SUCCESS,
				    "allocm() error for alignment=%zu, "
				    "size=%zu (%#zx): %d",
				    alignment, sz, sz, r);
				assert_zu_ge(rsz, sz,
				    "Real size smaller than expected for "
				    "alignment=%zu, size=%zu", alignment, sz);
				assert_zu_eq(nsz, rsz,
				    "nallocm()/allocm() rsize mismatch for "
				    "alignment=%zu, size=%zu", alignment, sz);
				assert_ptr_null(
				    (void *)((uintptr_t)ps[i] & (alignment-1)),
				    "%p inadequately aligned for"
				    " alignment=%zu, size=%zu", ps[i],
				    alignment, sz);
				sallocm(ps[i], &rsz, 0);
				total += rsz;
				if (total >= (MAXALIGN << 1))
					break;
			}
			for (i = 0; i < NITER; i++) {
				if (ps[i] != NULL) {
					dallocm(ps[i], 0);
					ps[i] = NULL;
				}
			}
		}
	}
}
TEST_END

int
main(void)
{

	return (test(
	    test_basic,
	    test_alignment_errors,
	    test_alignment_and_size));
}
