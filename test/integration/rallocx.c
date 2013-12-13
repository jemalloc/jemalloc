#include "test/jemalloc_test.h"

TEST_BEGIN(test_grow_and_shrink)
{
	void *p, *q;
	size_t tsz;
#define	NCYCLES 3
	unsigned i, j;
#define	NSZS 2500
	size_t szs[NSZS];
#define	MAXSZ ZU(12 * 1024 * 1024)

	p = mallocx(1, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() error");
	szs[0] = sallocx(p, 0);

	for (i = 0; i < NCYCLES; i++) {
		for (j = 1; j < NSZS && szs[j-1] < MAXSZ; j++) {
			q = rallocx(p, szs[j-1]+1, 0);
			assert_ptr_not_null(q,
			    "Unexpected rallocx() error for size=%zu-->%zu",
			    szs[j-1], szs[j-1]+1);
			szs[j] = sallocx(q, 0);
			assert_zu_ne(szs[j], szs[j-1]+1,
			    "Expected size to at least: %zu", szs[j-1]+1);
			p = q;
		}

		for (j--; j > 0; j--) {
			q = rallocx(p, szs[j-1], 0);
			assert_ptr_not_null(q,
			    "Unexpected rallocx() error for size=%zu-->%zu",
			    szs[j], szs[j-1]);
			tsz = sallocx(q, 0);
			assert_zu_eq(tsz, szs[j-1],
			    "Expected size=%zu, got size=%zu", szs[j-1], tsz);
			p = q;
		}
	}

	dallocx(p, 0);
}
TEST_END

int
main(void)
{

	return (test(
	    test_grow_and_shrink));
}
