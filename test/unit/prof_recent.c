#include "test/jemalloc_test.h"

#include "jemalloc/internal/prof_recent.h"

/* As specified in the shell script */
#define OPT_ALLOC_MAX 3

/* Invariant before and after every test (when config_prof is on) */
static void
confirm_prof_setup(tsd_t *tsd) {
	/* Options */
	assert_true(opt_prof, "opt_prof not on");
	assert_true(opt_prof_active, "opt_prof_active not on");
	assert_zd_eq(opt_prof_recent_alloc_max, OPT_ALLOC_MAX,
	    "opt_prof_recent_alloc_max not set correctly");

	/* Dynamics */
	assert_true(prof_active, "prof_active not on");
	assert_zd_eq(prof_recent_alloc_max_ctl_read(tsd), OPT_ALLOC_MAX,
	    "prof_recent_alloc_max not set correctly");
}

TEST_BEGIN(test_confirm_setup) {
	test_skip_if(!config_prof);
	confirm_prof_setup(tsd_fetch());
}
TEST_END

TEST_BEGIN(test_prof_recent_off) {
	test_skip_if(config_prof);

	const ssize_t past_ref = 0, future_ref = 0;
	const size_t len_ref = sizeof(ssize_t);

	ssize_t past = past_ref, future = future_ref;
	size_t len = len_ref;

#define ASSERT_SHOULD_FAIL(opt, a, b, c, d) do {			\
	assert_d_eq(mallctl("experimental.prof_recent." opt, a, b, c,	\
	    d), ENOENT, "Should return ENOENT when config_prof is off");\
	assert_zd_eq(past, past_ref, "output was touched");		\
	assert_zu_eq(len, len_ref, "output length was touched");	\
	assert_zd_eq(future, future_ref, "input was touched");		\
} while (0)

	ASSERT_SHOULD_FAIL("alloc_max", NULL, NULL, NULL, 0);
	ASSERT_SHOULD_FAIL("alloc_max", &past, &len, NULL, 0);
	ASSERT_SHOULD_FAIL("alloc_max", NULL, NULL, &future, len);
	ASSERT_SHOULD_FAIL("alloc_max", &past, &len, &future, len);

#undef ASSERT_SHOULD_FAIL
}
TEST_END

TEST_BEGIN(test_prof_recent_on) {
	test_skip_if(!config_prof);

	ssize_t past, future;
	size_t len = sizeof(ssize_t);

	tsd_t *tsd = tsd_fetch();

	confirm_prof_setup(tsd);

	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, NULL, 0), 0, "no-op mallctl should be allowed");
	confirm_prof_setup(tsd);

	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    &past, &len, NULL, 0), 0, "Read error");
	expect_zd_eq(past, OPT_ALLOC_MAX, "Wrong read result");
	future = OPT_ALLOC_MAX + 1;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, len), 0, "Write error");
	future = -1;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    &past, &len, &future, len), 0, "Read/write error");
	expect_zd_eq(past, OPT_ALLOC_MAX + 1, "Wrong read result");
	future = -2;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    &past, &len, &future, len), EINVAL,
	    "Invalid write should return EINVAL");
	expect_zd_eq(past, OPT_ALLOC_MAX + 1,
	    "Output should not be touched given invalid write");
	future = OPT_ALLOC_MAX;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    &past, &len, &future, len), 0, "Read/write error");
	expect_zd_eq(past, -1, "Wrong read result");
	future = OPT_ALLOC_MAX + 2;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    &past, &len, &future, len * 2), EINVAL,
	    "Invalid write should return EINVAL");
	expect_zd_eq(past, -1,
	    "Output should not be touched given invalid write");

	confirm_prof_setup(tsd);
}
TEST_END

/* Reproducible sequence of request sizes */
#define NTH_REQ_SIZE(n) ((n) * 97 + 101)

static void
confirm_malloc(tsd_t *tsd, void *p) {
	assert_ptr_not_null(p, "malloc failed unexpectedly");
	edata_t *e = emap_edata_lookup(TSDN_NULL, &emap_global, p);
	assert_ptr_not_null(e, "NULL edata for living pointer");
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	prof_recent_t *n = edata_prof_recent_alloc_get(tsd, e);
	assert_ptr_not_null(n, "Record in edata should not be NULL");
	expect_ptr_not_null(n->alloc_tctx,
	    "alloc_tctx in record should not be NULL");
	expect_ptr_eq(e, n->alloc_edata,
	    "edata pointer in record is not correct");
	expect_ptr_null(n->dalloc_tctx, "dalloc_tctx in record should be NULL");
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
}

static void
confirm_record_size(tsd_t *tsd, prof_recent_t *n, unsigned kth) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	expect_zu_eq(n->size, NTH_REQ_SIZE(kth),
	    "Recorded allocation size is wrong");
}

static void
confirm_record_living(tsd_t *tsd, prof_recent_t *n) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	expect_ptr_not_null(n->alloc_tctx,
	    "alloc_tctx in record should not be NULL");
	assert_ptr_not_null(n->alloc_edata,
	    "Recorded edata should not be NULL for living pointer");
	expect_ptr_eq(n, edata_prof_recent_alloc_get(tsd, n->alloc_edata),
	    "Record in edata is not correct");
	expect_ptr_null(n->dalloc_tctx, "dalloc_tctx in record should be NULL");
}

static void
confirm_record_released(tsd_t *tsd, prof_recent_t *n) {
	malloc_mutex_assert_owner(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	expect_ptr_not_null(n->alloc_tctx,
	    "alloc_tctx in record should not be NULL");
	expect_ptr_null(n->alloc_edata,
	    "Recorded edata should be NULL for released pointer");
	expect_ptr_not_null(n->dalloc_tctx,
	    "dalloc_tctx in record should not be NULL for released pointer");
}

TEST_BEGIN(test_prof_recent_alloc) {
	test_skip_if(!config_prof);

	bool b;
	unsigned i, c;
	size_t req_size;
	void *p;
	prof_recent_t *n;
	ssize_t future;

	tsd_t *tsd = tsd_fetch();

	confirm_prof_setup(tsd);

	/*
	 * First batch of 2 * OPT_ALLOC_MAX allocations.  After the
	 * (OPT_ALLOC_MAX - 1)'th allocation the recorded allocations should
	 * always be the last OPT_ALLOC_MAX allocations coming from here.
	 */
	for (i = 0; i < 2 * OPT_ALLOC_MAX; ++i) {
		req_size = NTH_REQ_SIZE(i);
		p = malloc(req_size);
		confirm_malloc(tsd, p);
		if (i < OPT_ALLOC_MAX - 1) {
			malloc_mutex_lock(tsd_tsdn(tsd),
			    &prof_recent_alloc_mtx);
			assert_ptr_ne(prof_recent_alloc_begin(tsd),
			    prof_recent_alloc_end(tsd),
			    "Empty recent allocation");
			malloc_mutex_unlock(tsd_tsdn(tsd),
			    &prof_recent_alloc_mtx);
			free(p);
			/*
			 * The recorded allocations may still include some
			 * other allocations before the test run started,
			 * so keep allocating without checking anything.
			 */
			continue;
		}
		c = 0;
		malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
		for (n = prof_recent_alloc_begin(tsd);
		    n != prof_recent_alloc_end(tsd);
		    n = prof_recent_alloc_next(tsd, n)) {
			++c;
			confirm_record_size(tsd, n, i + c - OPT_ALLOC_MAX);
			if (c == OPT_ALLOC_MAX) {
				confirm_record_living(tsd, n);
			} else {
				confirm_record_released(tsd, n);
			}
		}
		malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
		assert_u_eq(c, OPT_ALLOC_MAX,
		    "Incorrect total number of allocations");
		free(p);
	}

	confirm_prof_setup(tsd);

	b = false;
	assert_d_eq(mallctl("prof.active", NULL, NULL, &b, sizeof(bool)), 0,
	    "mallctl for turning off prof_active failed");

	/*
	 * Second batch of OPT_ALLOC_MAX allocations.  Since prof_active is
	 * turned off, this batch shouldn't be recorded.
	 */
	for (; i < 3 * OPT_ALLOC_MAX; ++i) {
		req_size = NTH_REQ_SIZE(i);
		p = malloc(req_size);
		assert_ptr_not_null(p, "malloc failed unexpectedly");
		c = 0;
		malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
		for (n = prof_recent_alloc_begin(tsd);
		    n != prof_recent_alloc_end(tsd);
		    n = prof_recent_alloc_next(tsd, n)) {
			confirm_record_size(tsd, n, c + OPT_ALLOC_MAX);
			confirm_record_released(tsd, n);
			++c;
		}
		malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
		assert_u_eq(c, OPT_ALLOC_MAX,
		    "Incorrect total number of allocations");
		free(p);
	}

	b = true;
	assert_d_eq(mallctl("prof.active", NULL, NULL, &b, sizeof(bool)), 0,
	    "mallctl for turning on prof_active failed");

	confirm_prof_setup(tsd);

	/*
	 * Third batch of OPT_ALLOC_MAX allocations.  Since prof_active is
	 * turned back on, they should be recorded, and in the list of recorded
	 * allocations they should follow the first batch rather than the
	 * second batch.
	 */
	for (; i < 4 * OPT_ALLOC_MAX; ++i) {
		req_size = NTH_REQ_SIZE(i);
		p = malloc(req_size);
		confirm_malloc(tsd, p);
		c = 0;
		malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
		for (n = prof_recent_alloc_begin(tsd);
		    n != prof_recent_alloc_end(tsd);
		    n = prof_recent_alloc_next(tsd, n)) {
			++c;
			confirm_record_size(tsd, n,
			    /* Is the allocation from the third batch? */
			    i + c - OPT_ALLOC_MAX >= 3 * OPT_ALLOC_MAX ?
			    /* If yes, then it's just recorded. */
			    i + c - OPT_ALLOC_MAX :
			    /*
			     * Otherwise, it should come from the first batch
			     * instead of the second batch.
			     */
			    i + c - 2 * OPT_ALLOC_MAX);
			if (c == OPT_ALLOC_MAX) {
				confirm_record_living(tsd, n);
			} else {
				confirm_record_released(tsd, n);
			}
		}
		malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
		assert_u_eq(c, OPT_ALLOC_MAX,
		    "Incorrect total number of allocations");
		free(p);
	}

	/* Increasing the limit shouldn't alter the list of records. */
	future = OPT_ALLOC_MAX + 1;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, sizeof(ssize_t)), 0, "Write error");
	c = 0;
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	for (n = prof_recent_alloc_begin(tsd);
	    n != prof_recent_alloc_end(tsd);
	    n = prof_recent_alloc_next(tsd, n)) {
		confirm_record_size(tsd, n, c + 3 * OPT_ALLOC_MAX);
		confirm_record_released(tsd, n);
		++c;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert_u_eq(c, OPT_ALLOC_MAX,
	    "Incorrect total number of allocations");

	/*
	 * Decreasing the limit shouldn't alter the list of records as long as
	 * the new limit is still no less than the length of the list.
	 */
	future = OPT_ALLOC_MAX;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, sizeof(ssize_t)), 0, "Write error");
	c = 0;
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	for (n = prof_recent_alloc_begin(tsd);
	    n != prof_recent_alloc_end(tsd);
	    n = prof_recent_alloc_next(tsd, n)) {
		confirm_record_size(tsd, n, c + 3 * OPT_ALLOC_MAX);
		confirm_record_released(tsd, n);
		++c;
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert_u_eq(c, OPT_ALLOC_MAX,
	    "Incorrect total number of allocations");

	/*
	 * Decreasing the limit should shorten the list of records if the new
	 * limit is less than the length of the list.
	 */
	future = OPT_ALLOC_MAX - 1;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, sizeof(ssize_t)), 0, "Write error");
	c = 0;
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	for (n = prof_recent_alloc_begin(tsd);
	    n != prof_recent_alloc_end(tsd);
	    n = prof_recent_alloc_next(tsd, n)) {
		++c;
		confirm_record_size(tsd, n, c + 3 * OPT_ALLOC_MAX);
		confirm_record_released(tsd, n);
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert_u_eq(c, OPT_ALLOC_MAX - 1,
	    "Incorrect total number of allocations");

	/* Setting to unlimited shouldn't alter the list of records. */
	future = -1;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, sizeof(ssize_t)), 0, "Write error");
	c = 0;
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	for (n = prof_recent_alloc_begin(tsd);
	    n != prof_recent_alloc_end(tsd);
	    n = prof_recent_alloc_next(tsd, n)) {
		++c;
		confirm_record_size(tsd, n, c + 3 * OPT_ALLOC_MAX);
		confirm_record_released(tsd, n);
	}
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert_u_eq(c, OPT_ALLOC_MAX - 1,
	    "Incorrect total number of allocations");

	/* Downshift to only one record. */
	future = 1;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, sizeof(ssize_t)), 0, "Write error");
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	n = prof_recent_alloc_begin(tsd);
	assert_ptr_ne(n, prof_recent_alloc_end(tsd), "Recent list is empty");
	confirm_record_size(tsd, n, 4 * OPT_ALLOC_MAX - 1);
	confirm_record_released(tsd, n);
	n = prof_recent_alloc_next(tsd, n);
	assert_ptr_eq(n, prof_recent_alloc_end(tsd),
	    "Recent list should be empty");
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);

	/* Completely turn off. */
	future = 0;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, sizeof(ssize_t)), 0, "Write error");
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert_ptr_eq(prof_recent_alloc_begin(tsd), prof_recent_alloc_end(tsd),
	    "Recent list should be empty");
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);

	/* Restore the settings. */
	future = OPT_ALLOC_MAX;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, sizeof(ssize_t)), 0, "Write error");
	malloc_mutex_lock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);
	assert_ptr_eq(prof_recent_alloc_begin(tsd), prof_recent_alloc_end(tsd),
	    "Recent list should be empty");
	malloc_mutex_unlock(tsd_tsdn(tsd), &prof_recent_alloc_mtx);

	confirm_prof_setup(tsd);
}
TEST_END

#undef NTH_REQ_SIZE

#define DUMP_OUT_SIZE 4096
static char dump_out[DUMP_OUT_SIZE];
static size_t dump_out_len = 0;

static void
test_dump_write_cb(void *not_used, const char *str) {
	size_t len = strlen(str);
	assert(dump_out_len + len < DUMP_OUT_SIZE);
	memcpy(dump_out + dump_out_len, str, len + 1);
	dump_out_len += len;
}

static void
call_dump() {
	static void *in[2] = {test_dump_write_cb, NULL};
	dump_out_len = 0;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_dump",
	    NULL, NULL, in, sizeof(in)), 0, "Dump mallctl raised error");
}

typedef struct {
	size_t size;
	bool released;
} confirm_record_t;

#define DUMP_ERROR "Dump output is wrong"

static void
confirm_record(const char *template,
    const confirm_record_t *records, const size_t n_records) {
	static const char *types[2] = {"alloc", "dalloc"};
	static char buf[64];

	/*
	 * The template string would be in the form of:
	 * "{\"recent_alloc_max\":XYZ,\"recent_alloc\":[]}",
	 * and dump_out would be in the form of:
	 * "{\"recent_alloc_max\":XYZ,\"recent_alloc\":[...]}".
	 * Using "- 2" serves to cut right before the ending "]}".
	 */
	assert_d_eq(memcmp(dump_out, template, strlen(template) - 2), 0,
	    DUMP_ERROR);
	assert_d_eq(memcmp(dump_out + strlen(dump_out) - 2,
	    template + strlen(template) - 2, 2), 0, DUMP_ERROR);

	const char *start = dump_out + strlen(template) - 2;
	const char *end = dump_out + strlen(dump_out) - 2;
	const confirm_record_t *record;
	for (record = records; record < records + n_records; ++record) {

#define ASSERT_CHAR(c) do {						\
	assert_true(start < end, DUMP_ERROR);				\
	assert_c_eq(*start++, c, DUMP_ERROR);				\
} while (0)

#define ASSERT_STR(s) do {						\
	const size_t len = strlen(s);					\
	assert_true(start + len <= end, DUMP_ERROR);			\
	assert_d_eq(memcmp(start, s, len), 0, DUMP_ERROR);		\
	start += len;							\
} while (0)

#define ASSERT_FORMATTED_STR(s, ...) do {				\
	malloc_snprintf(buf, sizeof(buf), s, __VA_ARGS__);		\
	ASSERT_STR(buf);						\
} while (0)

		if (record != records) {
			ASSERT_CHAR(',');
		}

		ASSERT_CHAR('{');

		ASSERT_STR("\"size\"");
		ASSERT_CHAR(':');
		ASSERT_FORMATTED_STR("%zu", record->size);
		ASSERT_CHAR(',');

		ASSERT_STR("\"usize\"");
		ASSERT_CHAR(':');
		ASSERT_FORMATTED_STR("%zu", sz_s2u(record->size));
		ASSERT_CHAR(',');

		ASSERT_STR("\"released\"");
		ASSERT_CHAR(':');
		ASSERT_STR(record->released ? "true" : "false");
		ASSERT_CHAR(',');

		const char **type = types;
		while (true) {
			ASSERT_FORMATTED_STR("\"%s_thread_uid\"", *type);
			ASSERT_CHAR(':');
			while (isdigit(*start)) {
				++start;
			}
			ASSERT_CHAR(',');

			ASSERT_FORMATTED_STR("\"%s_time\"", *type);
			ASSERT_CHAR(':');
			while (isdigit(*start)) {
				++start;
			}
			ASSERT_CHAR(',');

			ASSERT_FORMATTED_STR("\"%s_trace\"", *type);
			ASSERT_CHAR(':');
			ASSERT_CHAR('[');
			while (isdigit(*start) || *start == 'x' ||
			    (*start >= 'a' && *start <= 'f') ||
			    *start == '\"' || *start == ',') {
				++start;
			}
			ASSERT_CHAR(']');

			if (strcmp(*type, "dalloc") == 0) {
				break;
			}

			assert(strcmp(*type, "alloc") == 0);
			if (!record->released) {
				break;
			}

			ASSERT_CHAR(',');
			++type;
		}

		ASSERT_CHAR('}');

#undef ASSERT_FORMATTED_STR
#undef ASSERT_STR
#undef ASSERT_CHAR

	}
	assert_ptr_eq(record, records + n_records, DUMP_ERROR);
	assert_ptr_eq(start, end, DUMP_ERROR);
}

TEST_BEGIN(test_prof_recent_alloc_dump) {
	test_skip_if(!config_prof);

	tsd_t *tsd = tsd_fetch();
	confirm_prof_setup(tsd);

	ssize_t future;
	void *p, *q;
	confirm_record_t records[2];

	future = 0;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, sizeof(ssize_t)), 0, "Write error");
	call_dump();
	expect_str_eq(dump_out, "{\"recent_alloc_max\":0,\"recent_alloc\":[]}",
	    DUMP_ERROR);

	future = 2;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, sizeof(ssize_t)), 0, "Write error");
	call_dump();
	const char *template = "{\"recent_alloc_max\":2,\"recent_alloc\":[]}";
	expect_str_eq(dump_out, template, DUMP_ERROR);

	p = malloc(7);
	call_dump();
	records[0].size = 7;
	records[0].released = false;
	confirm_record(template, records, 1);

	q = malloc(17);
	call_dump();
	records[1].size = 17;
	records[1].released = false;
	confirm_record(template, records, 2);

	free(q);
	call_dump();
	records[1].released = true;
	confirm_record(template, records, 2);

	free(p);
	call_dump();
	records[0].released = true;
	confirm_record(template, records, 2);

	future = OPT_ALLOC_MAX;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &future, sizeof(ssize_t)), 0, "Write error");
	confirm_prof_setup(tsd);
}
TEST_END

#undef DUMP_ERROR
#undef DUMP_OUT_SIZE

#define N_THREADS 16
#define N_PTRS 512
#define N_CTLS 8
#define N_ITERS 2048
#define STRESS_ALLOC_MAX 4096

typedef struct {
	thd_t thd;
	size_t id;
	void *ptrs[N_PTRS];
	size_t count;
} thd_data_t;

static thd_data_t thd_data[N_THREADS];
static ssize_t test_max;

static void
test_write_cb(void *cbopaque, const char *str) {
	sleep_ns(1000 * 1000);
}

static void *
f_thread(void *arg) {
	const size_t thd_id = *(size_t *)arg;
	thd_data_t *data_p = thd_data + thd_id;
	assert(data_p->id == thd_id);
	data_p->count = 0;
	uint64_t rand = (uint64_t)thd_id;
	tsd_t *tsd = tsd_fetch();
	assert(test_max > 1);
	ssize_t last_max = -1;
	for (int i = 0; i < N_ITERS; i++) {
		rand = prng_range_u64(&rand, N_PTRS + N_CTLS * 5);
		assert(data_p->count <= N_PTRS);
		if (rand < data_p->count) {
			assert(data_p->count > 0);
			if (rand != data_p->count - 1) {
				assert(data_p->count > 1);
				void *temp = data_p->ptrs[rand];
				data_p->ptrs[rand] =
				    data_p->ptrs[data_p->count - 1];
				data_p->ptrs[data_p->count - 1] = temp;
			}
			free(data_p->ptrs[--data_p->count]);
		} else if (rand < N_PTRS) {
			assert(data_p->count < N_PTRS);
			data_p->ptrs[data_p->count++] = malloc(1);
		} else if (rand % 5 == 0) {
			prof_recent_alloc_dump(tsd, test_write_cb, NULL);
		} else if (rand % 5 == 1) {
			last_max = prof_recent_alloc_max_ctl_read(tsd);
		} else if (rand % 5 == 2) {
			last_max =
			    prof_recent_alloc_max_ctl_write(tsd, test_max * 2);
		} else if (rand % 5 == 3) {
			last_max =
			    prof_recent_alloc_max_ctl_write(tsd, test_max);
		} else {
			assert(rand % 5 == 4);
			last_max =
			    prof_recent_alloc_max_ctl_write(tsd, test_max / 2);
		}
		assert_zd_ge(last_max, -1, "Illegal last-N max");
	}

	while (data_p->count > 0) {
		free(data_p->ptrs[--data_p->count]);
	}

	return NULL;
}

TEST_BEGIN(test_prof_recent_stress) {
	test_skip_if(!config_prof);

	tsd_t *tsd = tsd_fetch();
	confirm_prof_setup(tsd);

	test_max = OPT_ALLOC_MAX;
	for (size_t i = 0; i < N_THREADS; i++) {
		thd_data_t *data_p = thd_data + i;
		data_p->id = i;
		thd_create(&data_p->thd, &f_thread, &data_p->id);
	}
	for (size_t i = 0; i < N_THREADS; i++) {
		thd_data_t *data_p = thd_data + i;
		thd_join(data_p->thd, NULL);
	}

	test_max = STRESS_ALLOC_MAX;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &test_max, sizeof(ssize_t)), 0, "Write error");
	for (size_t i = 0; i < N_THREADS; i++) {
		thd_data_t *data_p = thd_data + i;
		data_p->id = i;
		thd_create(&data_p->thd, &f_thread, &data_p->id);
	}
	for (size_t i = 0; i < N_THREADS; i++) {
		thd_data_t *data_p = thd_data + i;
		thd_join(data_p->thd, NULL);
	}

	test_max = OPT_ALLOC_MAX;
	assert_d_eq(mallctl("experimental.prof_recent.alloc_max",
	    NULL, NULL, &test_max, sizeof(ssize_t)), 0, "Write error");
	confirm_prof_setup(tsd);
}
TEST_END

#undef STRESS_ALLOC_MAX
#undef N_ITERS
#undef N_PTRS
#undef N_THREADS

int
main(void) {
	return test(
	    test_confirm_setup,
	    test_prof_recent_off,
	    test_prof_recent_on,
	    test_prof_recent_alloc,
	    test_prof_recent_alloc_dump,
	    test_prof_recent_stress);
}
