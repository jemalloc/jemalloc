#define	assert_cmp(t, a, b, cmp, neg_cmp, pri, fmt...) do {		\
	t a_ = (a);							\
	t b_ = (b);							\
	if (!(a_ cmp b_)) {						\
		p_test_fail(						\
		    "%s:%s:%d: Failed assertion: "			\
		    "(%s) "#cmp" (%s) --> "				\
		    "%"pri" "#neg_cmp" %"pri": ",			\
		    __func__, __FILE__, __LINE__,			\
		    #a, #b, a_, b_, fmt);				\
	}								\
} while (0)

#define	assert_ptr_eq(a, b, fmt...)	assert_cmp(void *, a, b, ==,	\
    !=, "p", fmt)
#define	assert_ptr_ne(a, b, fmt...)	assert_cmp(void *, a, b, !=,	\
    ==, "p", fmt)
#define	assert_ptr_null(a, fmt...)	assert_cmp(void *, a, NULL, ==,	\
    !=, "p", fmt)
#define	assert_ptr_not_null(a, fmt...)	assert_cmp(void *, a, NULL, !=,	\
    ==, "p", fmt)

#define	assert_c_eq(a, b, fmt...)	assert_cmp(char, a, b, ==, !=, "c", fmt)
#define	assert_c_ne(a, b, fmt...)	assert_cmp(char, a, b, !=, ==, "c", fmt)
#define	assert_c_lt(a, b, fmt...)	assert_cmp(char, a, b, <, >=, "c", fmt)
#define	assert_c_le(a, b, fmt...)	assert_cmp(char, a, b, <=, >, "c", fmt)
#define	assert_c_ge(a, b, fmt...)	assert_cmp(char, a, b, >=, <, "c", fmt)
#define	assert_c_gt(a, b, fmt...)	assert_cmp(char, a, b, >, <=, "c", fmt)

#define	assert_x_eq(a, b, fmt...)	assert_cmp(int, a, b, ==, !=, "#x", fmt)
#define	assert_x_ne(a, b, fmt...)	assert_cmp(int, a, b, !=, ==, "#x", fmt)
#define	assert_x_lt(a, b, fmt...)	assert_cmp(int, a, b, <, >=, "#x", fmt)
#define	assert_x_le(a, b, fmt...)	assert_cmp(int, a, b, <=, >, "#x", fmt)
#define	assert_x_ge(a, b, fmt...)	assert_cmp(int, a, b, >=, <, "#x", fmt)
#define	assert_x_gt(a, b, fmt...)	assert_cmp(int, a, b, >, <=, "#x", fmt)

#define	assert_d_eq(a, b, fmt...)	assert_cmp(int, a, b, ==, !=, "d", fmt)
#define	assert_d_ne(a, b, fmt...)	assert_cmp(int, a, b, !=, ==, "d", fmt)
#define	assert_d_lt(a, b, fmt...)	assert_cmp(int, a, b, <, >=, "d", fmt)
#define	assert_d_le(a, b, fmt...)	assert_cmp(int, a, b, <=, >, "d", fmt)
#define	assert_d_ge(a, b, fmt...)	assert_cmp(int, a, b, >=, <, "d", fmt)
#define	assert_d_gt(a, b, fmt...)	assert_cmp(int, a, b, >, <=, "d", fmt)

#define	assert_u_eq(a, b, fmt...)	assert_cmp(int, a, b, ==, !=, "u", fmt)
#define	assert_u_ne(a, b, fmt...)	assert_cmp(int, a, b, !=, ==, "u", fmt)
#define	assert_u_lt(a, b, fmt...)	assert_cmp(int, a, b, <, >=, "u", fmt)
#define	assert_u_le(a, b, fmt...)	assert_cmp(int, a, b, <=, >, "u", fmt)
#define	assert_u_ge(a, b, fmt...)	assert_cmp(int, a, b, >=, <, "u", fmt)
#define	assert_u_gt(a, b, fmt...)	assert_cmp(int, a, b, >, <=, "u", fmt)

#define	assert_zd_eq(a, b, fmt...)	assert_cmp(ssize_t, a, b, ==,	\
    !=, "zd", fmt)
#define	assert_zd_ne(a, b, fmt...)	assert_cmp(ssize_t, a, b, !=,	\
    ==, "zd", fmt)
#define	assert_zd_lt(a, b, fmt...)	assert_cmp(ssize_t, a, b, <,	\
    >=, "zd", fmt)
#define	assert_zd_le(a, b, fmt...)	assert_cmp(ssize_t, a, b, <=,	\
    >, "zd", fmt)
#define	assert_zd_ge(a, b, fmt...)	assert_cmp(ssize_t, a, b, >=,	\
    <, "zd", fmt)
#define	assert_zd_gt(a, b, fmt...)	assert_cmp(ssize_t, a, b, >,	\
    <=, "zd", fmt)

#define	assert_zu_eq(a, b, fmt...)	assert_cmp(size_t, a, b, ==,	\
    !=, "zu", fmt)
#define	assert_zu_ne(a, b, fmt...)	assert_cmp(size_t, a, b, !=,	\
    ==, "zu", fmt)
#define	assert_zu_lt(a, b, fmt...)	assert_cmp(size_t, a, b, <,	\
    >=, "zu", fmt)
#define	assert_zu_le(a, b, fmt...)	assert_cmp(size_t, a, b, <=,	\
    >, "zu", fmt)
#define	assert_zu_ge(a, b, fmt...)	assert_cmp(size_t, a, b, >=,	\
    <, "zu", fmt)
#define	assert_zu_gt(a, b, fmt...)	assert_cmp(size_t, a, b, >,	\
    <=, "zu", fmt)

#define	assert_d32_eq(a, b, fmt...)	assert_cmp(int32_t, a, b, ==,	\
    !=, PRId32, fmt)
#define	assert_d32_ne(a, b, fmt...)	assert_cmp(int32_t, a, b, !=,	\
    ==, PRId32, fmt)
#define	assert_d32_lt(a, b, fmt...)	assert_cmp(int32_t, a, b, <,	\
    >=, PRId32, fmt)
#define	assert_d32_le(a, b, fmt...)	assert_cmp(int32_t, a, b, <=,	\
    >, PRId32, fmt)
#define	assert_d32_ge(a, b, fmt...)	assert_cmp(int32_t, a, b, >=,	\
    <, PRId32, fmt)
#define	assert_d32_gt(a, b, fmt...)	assert_cmp(int32_t, a, b, >,	\
    <=, PRId32, fmt)

#define	assert_u32_eq(a, b, fmt...)	assert_cmp(uint32_t, a, b, ==,	\
    !=, PRIu32, fmt)
#define	assert_u32_ne(a, b, fmt...)	assert_cmp(uint32_t, a, b, !=,	\
    ==, PRIu32, fmt)
#define	assert_u32_lt(a, b, fmt...)	assert_cmp(uint32_t, a, b, <,	\
    >=, PRIu32, fmt)
#define	assert_u32_le(a, b, fmt...)	assert_cmp(uint32_t, a, b, <=,	\
    >, PRIu32, fmt)
#define	assert_u32_ge(a, b, fmt...)	assert_cmp(uint32_t, a, b, >=,	\
    <, PRIu32, fmt)
#define	assert_u32_gt(a, b, fmt...)	assert_cmp(uint32_t, a, b, >,	\
    <=, PRIu32, fmt)

#define	assert_d64_eq(a, b, fmt...)	assert_cmp(int64_t, a, b, ==,	\
    !=, PRId64, fmt)
#define	assert_d64_ne(a, b, fmt...)	assert_cmp(int64_t, a, b, !=,	\
    ==, PRId64, fmt)
#define	assert_d64_lt(a, b, fmt...)	assert_cmp(int64_t, a, b, <,	\
    >=, PRId64, fmt)
#define	assert_d64_le(a, b, fmt...)	assert_cmp(int64_t, a, b, <=,	\
    >, PRId64, fmt)
#define	assert_d64_ge(a, b, fmt...)	assert_cmp(int64_t, a, b, >=,	\
    <, PRId64, fmt)
#define	assert_d64_gt(a, b, fmt...)	assert_cmp(int64_t, a, b, >,	\
    <=, PRId64, fmt)

#define	assert_u64_eq(a, b, fmt...)	assert_cmp(uint64_t, a, b, ==,	\
    !=, PRIu64, fmt)
#define	assert_u64_ne(a, b, fmt...)	assert_cmp(uint64_t, a, b, !=,	\
    ==, PRIu64, fmt)
#define	assert_u64_lt(a, b, fmt...)	assert_cmp(uint64_t, a, b, <,	\
    >=, PRIu64, fmt)
#define	assert_u64_le(a, b, fmt...)	assert_cmp(uint64_t, a, b, <=,	\
    >, PRIu64, fmt)
#define	assert_u64_ge(a, b, fmt...)	assert_cmp(uint64_t, a, b, >=,	\
    <, PRIu64, fmt)
#define	assert_u64_gt(a, b, fmt...)	assert_cmp(uint64_t, a, b, >,	\
    <=, PRIu64, fmt)

#define	assert_true(a, fmt...) do {					\
	bool a_ = (a);							\
	if (!(a_ == true)) {						\
		p_test_fail(						\
		    "%s:%s:%d: Failed assertion: "			\
		    "(%s) == true --> %s != true: ",			\
		    __func__, __FILE__, __LINE__,			\
		    #a, a_ ? "true" : "false", fmt);			\
	}								\
} while (0)
#define	assert_false(a, fmt...) do {					\
	bool a_ = (a);							\
	if (!(a_ == false)) {						\
		p_test_fail(						\
		    "%s:%s:%d: Failed assertion: "			\
		    "(%s) == false --> %s != false: ",			\
		    __func__, __FILE__, __LINE__,			\
		    #a, a_ ? "true" : "false", fmt);			\
	}								\
} while (0)

#define	assert_str_eq(a, b, fmt...) do {				\
	if (strcmp((a), (b))) {						\
		p_test_fail(						\
		    "%s:%s:%d: Failed assertion: "			\
		    "(%s) same as (%s) --> "				\
		    "\"%s\" differs from \"%s\": ",			\
		    __func__, __FILE__, __LINE__, #a, #b, a, b, fmt);	\
	}								\
} while (0)
#define	assert_str_ne(a, b, fmt...) do {				\
	if (!strcmp((a), (b))) {					\
		p_test_fail(						\
		    "%s:%s:%d: Failed assertion: "			\
		    "(%s) differs from (%s) --> "			\
		    "\"%s\" same as \"%s\": ",				\
		    __func__, __FILE__, __LINE__, #a, #b, a, b, fmt);	\
	}								\
} while (0)

/*
 * If this enum changes, corresponding changes in test/test.sh.in are also
 * necessary.
 */
typedef enum {
	test_status_pass = 0,
	test_status_skip = 1,
	test_status_fail = 2,

	test_status_count = 3
} test_status_t;

typedef void (test_t)(void);

#define	TEST_BEGIN(f)							\
static void								\
f(void)									\
{									\
	p_test_init(#f);

#define	TEST_END							\
	goto label_test_end;						\
label_test_end:								\
	p_test_fini();							\
}

#define	test(tests...)							\
	p_test(tests, NULL)

#define	test_skip_if(e) do {						\
	if (e) {							\
		test_skip("%s:%s:%d: Test skipped: (%s)",		\
		    __func__, __FILE__, __LINE__, #e);			\
		goto label_test_end;					\
	}								\
} while (0)

void	test_skip(const char *format, ...) JEMALLOC_ATTR(format(printf, 1, 2));
void	test_fail(const char *format, ...) JEMALLOC_ATTR(format(printf, 1, 2));

/* For private use by macros. */
test_status_t	p_test(test_t* t, ...);
void	p_test_init(const char *name);
void	p_test_fini(void);
void	p_test_fail(const char *format, ...);
