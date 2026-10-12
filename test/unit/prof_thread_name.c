#include "test/jemalloc_test.h"

#include "jemalloc/internal/prof_sys.h"

static void
mallctl_thread_name_get_impl(
    const char *thread_name_expected, const char *func, int line) {
	const char *thread_name_old;
	size_t      sz;

	sz = sizeof(thread_name_old);
	expect_d_eq(
	    mallctl("thread.prof.name", (void *)&thread_name_old, &sz, NULL, 0),
	    0, "%s():%d: Unexpected mallctl failure reading thread.prof.name",
	    func, line);
	expect_str_eq(thread_name_old, thread_name_expected,
	    "%s():%d: Unexpected thread.prof.name value", func, line);
}

static void
mallctl_thread_name_set_impl(
    const char *thread_name, const char *func, int line) {
	expect_d_eq(mallctl("thread.prof.name", NULL, NULL,
	                (void *)&thread_name, sizeof(thread_name)),
	    0, "%s():%d: Unexpected mallctl failure writing thread.prof.name",
	    func, line);
	mallctl_thread_name_get_impl(thread_name, func, line);
}

#define mallctl_thread_name_get(a)                                             \
	mallctl_thread_name_get_impl(a, __func__, __LINE__)

#define mallctl_thread_name_set(a)                                             \
	mallctl_thread_name_set_impl(a, __func__, __LINE__)

TEST_BEGIN(test_prof_thread_name_validation) {
	test_skip_if(!config_prof);
	test_skip_if(opt_prof_sys_thread_name);

	mallctl_thread_name_get("");

	const char *test_name1 = "test case1";
	mallctl_thread_name_set(test_name1);

	/* Test name longer than the max len. */
	char long_name[] =
	    "test case longer than expected; test case longer than expected";
	expect_zu_gt(strlen(long_name), PROF_THREAD_NAME_MAX_LEN,
	    "Long test name not long enough");
	const char *test_name_long = long_name;
	expect_d_eq(mallctl("thread.prof.name", NULL, NULL,
	                (void *)&test_name_long, sizeof(test_name_long)),
	    0, "Unexpected mallctl failure from thread.prof.name");
	/* Long name cut to match. */
	long_name[PROF_THREAD_NAME_MAX_LEN - 1] = '\0';
	mallctl_thread_name_get(test_name_long);

	/* NULL input shouldn't be allowed. */
	const char *test_name2 = NULL;
	expect_d_eq(mallctl("thread.prof.name", NULL, NULL, (void *)&test_name2,
	                sizeof(test_name2)),
	    EINVAL, "Unexpected mallctl result writing to thread.prof.name");

	/* '\n' shouldn't be allowed. */
	const char *test_name3 = "test\ncase";
	expect_d_eq(mallctl("thread.prof.name", NULL, NULL, (void *)&test_name3,
	                sizeof(test_name3)),
	    EINVAL,
	    "Unexpected mallctl result writing \"%s\" to thread.prof.name",
	    test_name3);

	/* Simultaneous read/write shouldn't be allowed. */
	const char *thread_name_old;
	size_t      sz = sizeof(thread_name_old);
	expect_d_eq(mallctl("thread.prof.name", (void *)&thread_name_old, &sz,
	                (void *)&test_name1, sizeof(test_name1)),
	    EPERM, "Unexpected mallctl result from thread.prof.name");

	mallctl_thread_name_set("");
}
TEST_END

static void *
thd_start(void *varg) {
	unsigned thd_ind = *(unsigned *)varg;
	char     thread_name[16] = "";
	unsigned i;

	malloc_snprintf(thread_name, sizeof(thread_name), "thread %u", thd_ind);

	mallctl_thread_name_get("");
	mallctl_thread_name_set(thread_name);

#define NRESET 25
	for (i = 0; i < NRESET; i++) {
		expect_d_eq(mallctl("prof.reset", NULL, NULL, NULL, 0), 0,
		    "Unexpected error while resetting heap profile data");
		mallctl_thread_name_get(thread_name);
	}

	mallctl_thread_name_set(thread_name);
	mallctl_thread_name_set("");

	return NULL;
#undef NRESET
}

TEST_BEGIN(test_prof_thread_name_threaded) {
	test_skip_if(!config_prof);
	test_skip_if(opt_prof_sys_thread_name);

#define NTHREADS 4
	thd_t    thds[NTHREADS];
	unsigned thd_args[NTHREADS];
	unsigned i;

	for (i = 0; i < NTHREADS; i++) {
		thd_args[i] = i;
		thd_create(&thds[i], thd_start, (void *)&thd_args[i]);
	}
	for (i = 0; i < NTHREADS; i++) {
		thd_join(thds[i], NULL);
	}
#undef NTHREADS
}
TEST_END

#define NRENAMERS 8
#define NDUMPS 500
static const char *rename_names[] = {"aaaaaaaaaaaaaaa", "b"};
static char        dump_buf[1u << 20];
static size_t      dump_len;
static atomic_u_t  renamers_ready;
static atomic_b_t  renamers_stop;

static int
prof_dump_open_file_intercept(const char *filename, int mode) {
	return open("/dev/null", O_WRONLY);
}

static ssize_t
prof_dump_write_file_intercept(int fd, const void *s, size_t len) {
	assert_zu_lt(dump_len + len, sizeof(dump_buf), "Dump capture overflow");
	memcpy(&dump_buf[dump_len], s, len);
	dump_len += len;
	return (ssize_t)len;
}

static void *
thd_rename_start(void *unused) {
	size_t mib[3];
	size_t miblen = sizeof(mib) / sizeof(mib[0]);
	expect_d_eq(mallctlnametomib("thread.prof.name", mib, &miblen), 0,
	    "Unexpected mallctlnametomib failure");
	for (unsigned i = 0; !atomic_load_b(&renamers_stop, ATOMIC_ACQUIRE);
	     i++) {
		expect_d_eq(mallctlbymib(mib, miblen, NULL, NULL,
		                (void *)&rename_names[i % 2],
		                sizeof(const char *)),
		    0, "Unexpected mallctl failure writing thread.prof.name");
		if (i == 0) {
			atomic_fetch_add_u(&renamers_ready, 1, ATOMIC_RELEASE);
		}
	}
	return NULL;
}

static bool
name_is_one_of(const char *s, size_t len) {
	for (unsigned i = 0; i < 2; i++) {
		if (strlen(rename_names[i]) == len
		    && memcmp(s, rename_names[i], len) == 0) {
			return true;
		}
	}
	return false;
}

/*
 * The dump is complete and every named "  t<uid>: ... [..: ..] <name>" line
 * carries exactly one of rename_names.
 */
static bool
dump_is_intact(void) {
	if (dump_len == 0 || dump_buf[dump_len - 1] != '\n') {
		return false;
	}
	unsigned named = 0;
	for (const char *line = strstr(dump_buf, "\n  t"); line != NULL;
	     line = strstr(line + 1, "\n  t")) {
		const char *eol = strchr(line + 1, '\n');
		const char *p = memchr(line, ']', (size_t)(eol - line));
		if (p == NULL) {
			return false;
		}
		if (p + 1 == eol) {
			continue;
		}
		if (p[1] != ' ' || !name_is_one_of(p + 2, (size_t)(eol - (p + 2)))) {
			return false;
		}
		named++;
	}
	return named == NRENAMERS;
}

TEST_BEGIN(test_prof_thread_name_dump_concurrent_rename) {
	test_skip_if(!config_prof);
	test_skip_if(opt_prof_sys_thread_name);

	prof_dump_open_file_t  *open_file_orig = prof_dump_open_file;
	prof_dump_write_file_t *write_file_orig = prof_dump_write_file;
	prof_dump_open_file = prof_dump_open_file_intercept;
	prof_dump_write_file = prof_dump_write_file_intercept;

	thd_t thds[NRENAMERS];
	atomic_store_u(&renamers_ready, 0, ATOMIC_RELAXED);
	atomic_store_b(&renamers_stop, false, ATOMIC_RELAXED);
	for (unsigned i = 0; i < NRENAMERS; i++) {
		thd_create(&thds[i], thd_rename_start, NULL);
	}
	while (atomic_load_u(&renamers_ready, ATOMIC_ACQUIRE) < NRENAMERS) {
	}

	const char *filename = "test_filename";
	unsigned    corrupted = 0;
	for (unsigned i = 0; i < NDUMPS; i++) {
		dump_len = 0;
		expect_d_eq(mallctl("prof.dump", NULL, NULL, (void *)&filename,
		                sizeof(filename)),
		    0, "Unexpected mallctl failure during prof.dump");
		dump_buf[dump_len] = '\0';
		corrupted += !dump_is_intact();
	}

	atomic_store_b(&renamers_stop, true, ATOMIC_RELEASE);
	for (unsigned i = 0; i < NRENAMERS; i++) {
		thd_join(thds[i], NULL);
	}
	prof_dump_open_file = open_file_orig;
	prof_dump_write_file = write_file_orig;
	expect_u_eq(corrupted, 0, "%u of %u heap dumps are malformed",
	    corrupted, NDUMPS);
}
TEST_END

int
main(void) {
	return test(test_prof_thread_name_validation,
	    test_prof_thread_name_threaded,
	    test_prof_thread_name_dump_concurrent_rename);
}
