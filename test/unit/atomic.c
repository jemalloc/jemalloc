#include "test/jemalloc_test.h"

#define	TEST_STRUCT(p, t)						\
struct p##_test_s {							\
	t	accum0;							\
	t	x;							\
};									\
typedef struct p##_test_s p##_test_t;

#define	TEST_BODY(p, t, PRI) do {					\
	const p##_test_t tests[] = {					\
		{-1, -1},						\
		{-1,  0},						\
		{-1,  1},						\
									\
		{ 0, -1},						\
		{ 0,  0},						\
		{ 0,  1},						\
									\
		{ 1, -1},						\
		{ 1,  0},						\
		{ 1,  1},						\
									\
		{0, -(1 << 22)},					\
		{0, (1 << 22)},						\
		{(1 << 22), -(1 << 22)},				\
		{(1 << 22), (1 << 22)}					\
	};								\
	unsigned i;							\
									\
	for (i = 0; i < sizeof(tests)/sizeof(p##_test_t); i++) {	\
		t accum = tests[i].accum0;				\
		assert_u64_eq(atomic_read_##p(&accum), tests[i].accum0,	\
		    "i=%u", i);						\
		assert_u64_eq(atomic_add_##p(&accum, tests[i].x),	\
		    tests[i].accum0 + tests[i].x,			\
		    "i=%u, accum=%#"PRI", x=%#"PRI,			\
		    i, tests[i].accum0, tests[i].x);			\
		assert_u64_eq(atomic_read_##p(&accum), accum,		\
		    "i=%u", i);						\
									\
		accum = tests[i].accum0;				\
		assert_u64_eq(atomic_sub_##p(&accum, tests[i].x),	\
		    tests[i].accum0 - tests[i].x,			\
		    "i=%u, accum=%#"PRI", x=%#"PRI,			\
		    i, tests[i].accum0, tests[i].x);			\
		assert_u64_eq(atomic_read_##p(&accum), accum,		\
		    "i=%u", i);						\
	}								\
} while (0)

TEST_STRUCT(uint64, uint64_t)
TEST_BEGIN(test_atomic_uint64)
{

#if !(LG_SIZEOF_PTR == 3 || LG_SIZEOF_INT == 3)
	test_skip("64-bit atomic operations not supported");
#else
	TEST_BODY(uint64, uint64_t, PRIx64);
#endif
}
TEST_END

TEST_STRUCT(uint32, uint32_t)
TEST_BEGIN(test_atomic_uint32)
{

	TEST_BODY(uint32, uint32_t, PRIx32);
}
TEST_END

TEST_STRUCT(z, size_t)
TEST_BEGIN(test_atomic_z)
{

	TEST_BODY(z, size_t, "zx");
}
TEST_END

TEST_STRUCT(u, unsigned)
TEST_BEGIN(test_atomic_u)
{

	TEST_BODY(u, unsigned, "x");
}
TEST_END

int
main(void)
{

	return (test(
	    test_atomic_uint64,
	    test_atomic_uint32,
	    test_atomic_z,
	    test_atomic_u));
}
