#include "test/jemalloc_test.h"
#include "jemalloc/internal/prof_stats.h"

typedef struct {
	char  *buf;
	size_t len;
	size_t capacity;
} stats_buf_t;

static void
stats_buf_init(stats_buf_t *sbuf) {
	/* 1MB is enough for the small number of arenas used by these tests. */
	sbuf->capacity = 1 << 20;
	sbuf->buf = mallocx(sbuf->capacity, MALLOCX_TCACHE_NONE);
	assert_ptr_not_null(sbuf->buf, "Failed to allocate stats buffer");
	sbuf->len = 0;
	sbuf->buf[0] = '\0';
}

static void
stats_buf_fini(stats_buf_t *sbuf) {
	dallocx(sbuf->buf, MALLOCX_TCACHE_NONE);
}

static void
stats_buf_write_cb(void *opaque, const char *str) {
	stats_buf_t *sbuf = (stats_buf_t *)opaque;
	size_t       slen = strlen(str);

	if (sbuf->len + slen + 1 > sbuf->capacity) {
		return;
	}
	memcpy(&sbuf->buf[sbuf->len], str, slen + 1);
	sbuf->len += slen;
}

static const char  *global_mutex_names[] = {"background_thread",
     "max_per_bg_thd", "ctl", "prof", "prof_thds_data", "prof_dump",
     "prof_recent_alloc", "prof_recent_dump", "prof_stats"};
static const size_t num_global_mutexes = sizeof(global_mutex_names)
    / sizeof(global_mutex_names[0]);

static const char  *arena_mutex_names[] = {"large", "extent_avail",
     "extents_dirty", "extents_muzzy", "extents_retained", "extents_pinned",
     "decay_dirty", "decay_muzzy", "base", "tcache_list", "hpa_shard",
     "hpa_shard_grow", "hpa_sec", "pac_sec"};
static const size_t num_arena_mutexes = sizeof(arena_mutex_names)
    / sizeof(arena_mutex_names[0]);

typedef struct {
	const char *begin;
	const char *end;
} json_fragment_t;

static const char *
json_skip_ws(const char *cur, const char *end) {
	while (cur < end
	    && (*cur == ' ' || *cur == '\t' || *cur == '\n' || *cur == '\r')) {
		cur++;
	}
	return cur;
}

static const char *
json_string_end(const char *begin, const char *end) {
	if (begin >= end || *begin != '"') {
		return NULL;
	}
	for (const char *cur = begin + 1; cur < end; cur++) {
		if (*cur == '\\') {
			cur++;
			if (cur >= end) {
				return NULL;
			}
		} else if (*cur == '"') {
			return cur + 1;
		}
	}
	return NULL;
}

static const char *
json_value_end(const char *begin, const char *end) {
	begin = json_skip_ws(begin, end);
	if (begin >= end) {
		return NULL;
	}
	if (*begin == '"') {
		return json_string_end(begin, end);
	}
	if (*begin != '{' && *begin != '[') {
		const char *cur = begin;
		while (cur < end && *cur != ',' && *cur != '}' && *cur != ']') {
			cur++;
		}
		while (cur > begin
		    && (cur[-1] == ' ' || cur[-1] == '\t' || cur[-1] == '\n'
		        || cur[-1] == '\r')) {
			cur--;
		}
		return cur;
	}

	char opening = *begin;
	char closing = opening == '{' ? '}' : ']';
	unsigned depth = 0;
	for (const char *cur = begin; cur < end; cur++) {
		if (*cur == '"') {
			cur = json_string_end(cur, end);
			if (cur == NULL) {
				return NULL;
			}
			cur--;
			continue;
		}
		if (*cur == opening) {
			depth++;
		} else if (*cur == closing && --depth == 0) {
			return cur + 1;
		}
	}
	return NULL;
}

static bool
json_fragment_init(const char *begin, const char *end, json_fragment_t *out) {
	begin = json_skip_ws(begin, end);
	const char *value_end = json_value_end(begin, end);
	if (value_end == NULL) {
		return true;
	}
	out->begin = begin;
	out->end = value_end;
	return false;
}

static bool
json_object_member(json_fragment_t object, const char *key,
    json_fragment_t *value) {
	if (object.begin >= object.end || *object.begin != '{') {
		return true;
	}
	const char *cur = object.begin + 1;
	while (true) {
		cur = json_skip_ws(cur, object.end);
		if (cur >= object.end || *cur == '}') {
			return true;
		}
		const char *key_end = json_string_end(cur, object.end);
		if (key_end == NULL) {
			return true;
		}
		size_t key_len = (size_t)(key_end - cur - 2);
		bool matches = strlen(key) == key_len
		    && strncmp(cur + 1, key, key_len) == 0;
		cur = json_skip_ws(key_end, object.end);
		if (cur >= object.end || *cur++ != ':') {
			return true;
		}
		cur = json_skip_ws(cur, object.end);
		const char *value_end = json_value_end(cur, object.end);
		if (value_end == NULL) {
			return true;
		}
		if (matches) {
			value->begin = cur;
			value->end = value_end;
			return false;
		}
		cur = json_skip_ws(value_end, object.end);
		if (cur < object.end && *cur == ',') {
			cur++;
		} else if (cur >= object.end || *cur != '}') {
			return true;
		}
	}
}

static size_t
json_object_size(json_fragment_t object) {
	if (object.begin >= object.end || *object.begin != '{') {
		return SIZE_T_MAX;
	}
	size_t count = 0;
	const char *cur = object.begin + 1;
	while (true) {
		cur = json_skip_ws(cur, object.end);
		if (cur < object.end && *cur == '}') {
			return count;
		}
		const char *key_end = json_string_end(cur, object.end);
		if (key_end == NULL) {
			return SIZE_T_MAX;
		}
		cur = json_skip_ws(key_end, object.end);
		if (cur >= object.end || *cur++ != ':') {
			return SIZE_T_MAX;
		}
		cur = json_skip_ws(cur, object.end);
		cur = json_value_end(cur, object.end);
		if (cur == NULL) {
			return SIZE_T_MAX;
		}
		count++;
		cur = json_skip_ws(cur, object.end);
		if (cur < object.end && *cur == ',') {
			cur++;
		} else if (cur >= object.end || *cur != '}') {
			return SIZE_T_MAX;
		}
	}
}

static bool
json_array_element(json_fragment_t array, size_t ind, json_fragment_t *value) {
	if (array.begin >= array.end || *array.begin != '[') {
		return true;
	}
	size_t current = 0;
	const char *cur = array.begin + 1;
	while (true) {
		cur = json_skip_ws(cur, array.end);
		if (cur >= array.end || *cur == ']') {
			return true;
		}
		const char *value_end = json_value_end(cur, array.end);
		if (value_end == NULL) {
			return true;
		}
		if (current++ == ind) {
			value->begin = cur;
			value->end = value_end;
			return false;
		}
		cur = json_skip_ws(value_end, array.end);
		if (cur < array.end && *cur == ',') {
			cur++;
		} else if (cur >= array.end || *cur != ']') {
			return true;
		}
	}
}

static size_t
json_array_size(json_fragment_t array) {
	if (array.begin >= array.end || *array.begin != '[') {
		return SIZE_T_MAX;
	}
	size_t count = 0;
	const char *cur = array.begin + 1;
	while (true) {
		cur = json_skip_ws(cur, array.end);
		if (cur < array.end && *cur == ']') {
			return count;
		}
		cur = json_value_end(cur, array.end);
		if (cur == NULL) {
			return SIZE_T_MAX;
		}
		count++;
		cur = json_skip_ws(cur, array.end);
		if (cur < array.end && *cur == ',') {
			cur++;
		} else if (cur >= array.end || *cur != ']') {
			return SIZE_T_MAX;
		}
	}
}

static bool
json_fragment_uint64(json_fragment_t value, uint64_t *result) {
	char *end;
	errno = 0;
	uint64_t parsed = strtoull(value.begin, &end, 10);
	if (errno != 0 || end != value.end) {
		return true;
	}
	*result = parsed;
	return false;
}

static bool
json_fragment_int64(json_fragment_t value, int64_t *result) {
	char *end;
	errno = 0;
	int64_t parsed = strtoll(value.begin, &end, 10);
	if (errno != 0 || end != value.end) {
		return true;
	}
	*result = parsed;
	return false;
}

static bool
json_fragment_string_eq(json_fragment_t value, const char *expected) {
	if (value.end - value.begin < 2 || *value.begin != '"'
	    || value.end[-1] != '"') {
		return false;
	}
	size_t len = (size_t)(value.end - value.begin - 2);
	return strlen(expected) == len
	    && strncmp(value.begin + 1, expected, len) == 0;
}

static void
expect_json_object_has_keys(json_fragment_t object, const char *const *keys,
    size_t nkeys, const char *name) {
	for (size_t i = 0; i < nkeys; i++) {
		json_fragment_t ignored;
		expect_false(json_object_member(object, keys[i], &ignored),
		    "%s is missing key %s", name, keys[i]);
	}
}

static void
expect_json_object_keys(json_fragment_t object, const char *const *keys,
    size_t nkeys, const char *name) {
	expect_zu_eq(json_object_size(object), nkeys,
	    "%s has an unexpected number of keys", name);
	expect_json_object_has_keys(object, keys, nkeys, name);
}

typedef enum {
	ctl_field_size,
	ctl_field_uint64,
	ctl_field_uint32,
	ctl_field_unsigned,
	ctl_field_ssize,
	ctl_field_string,
} ctl_field_type_t;

typedef struct {
	const char       *json_key;
	const char       *ctl_suffix;
	ctl_field_type_t  type;
} ctl_field_t;

static void
expect_json_ctl_fields(json_fragment_t object, const char *ctl_prefix,
    const ctl_field_t *fields, size_t nfields) {
	for (size_t i = 0; i < nfields; i++) {
		const ctl_field_t *field = &fields[i];
		json_fragment_t value;
		bool missing = json_object_member(object, field->json_key, &value);
		expect_false(missing, "JSON is missing %s", field->json_key);
		if (missing) {
			continue;
		}

		char ctl_name[256];
		size_t written = malloc_snprintf(ctl_name, sizeof(ctl_name),
		    "%s.%s", ctl_prefix, field->ctl_suffix);
		expect_zu_lt(written, sizeof(ctl_name), "mallctl name is too long");

		size_t sz;
		uint64_t json_u64 = 0;
		int64_t json_i64 = 0;
		switch (field->type) {
		case ctl_field_size: {
			size_t expected;
			sz = sizeof(expected);
			expect_d_eq(mallctl(ctl_name, &expected, &sz, NULL, 0), 0,
			    "mallctl failed for %s", ctl_name);
			expect_false(json_fragment_uint64(value, &json_u64),
			    "JSON value for %s is not unsigned", field->json_key);
			expect_zu_eq((size_t)json_u64, expected,
			    "JSON value does not match %s", ctl_name);
			break;
		}
		case ctl_field_uint64: {
			uint64_t expected;
			sz = sizeof(expected);
			expect_d_eq(mallctl(ctl_name, &expected, &sz, NULL, 0), 0,
			    "mallctl failed for %s", ctl_name);
			expect_false(json_fragment_uint64(value, &json_u64),
			    "JSON value for %s is not unsigned", field->json_key);
			expect_u64_eq(json_u64, expected,
			    "JSON value does not match %s", ctl_name);
			break;
		}
		case ctl_field_uint32: {
			uint32_t expected;
			sz = sizeof(expected);
			expect_d_eq(mallctl(ctl_name, &expected, &sz, NULL, 0), 0,
			    "mallctl failed for %s", ctl_name);
			expect_false(json_fragment_uint64(value, &json_u64),
			    "JSON value for %s is not unsigned", field->json_key);
			expect_u32_eq((uint32_t)json_u64, expected,
			    "JSON value does not match %s", ctl_name);
			break;
		}
		case ctl_field_unsigned: {
			unsigned expected;
			sz = sizeof(expected);
			expect_d_eq(mallctl(ctl_name, &expected, &sz, NULL, 0), 0,
			    "mallctl failed for %s", ctl_name);
			expect_false(json_fragment_uint64(value, &json_u64),
			    "JSON value for %s is not unsigned", field->json_key);
			expect_u_eq((unsigned)json_u64, expected,
			    "JSON value does not match %s", ctl_name);
			break;
		}
		case ctl_field_ssize: {
			ssize_t expected;
			sz = sizeof(expected);
			expect_d_eq(mallctl(ctl_name, &expected, &sz, NULL, 0), 0,
			    "mallctl failed for %s", ctl_name);
			expect_false(json_fragment_int64(value, &json_i64),
			    "JSON value for %s is not signed", field->json_key);
			expect_zd_eq((ssize_t)json_i64, expected,
			    "JSON value does not match %s", ctl_name);
			break;
		}
		case ctl_field_string: {
			const char *expected;
			sz = sizeof(expected);
			expect_d_eq(mallctl(ctl_name, &expected, &sz, NULL, 0), 0,
			    "mallctl failed for %s", ctl_name);
			expect_true(json_fragment_string_eq(value, expected),
			    "JSON value does not match %s", ctl_name);
			break;
		}
		}
	}
}

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

static void
expect_json_uint_eq(json_fragment_t object, const char *key,
    uint64_t expected, const char *name) {
	json_fragment_t value;
	bool missing = json_object_member(object, key, &value);
	expect_false(missing, "%s is missing %s", name, key);
	if (missing) {
		return;
	}
	uint64_t actual = 0;
	expect_false(json_fragment_uint64(value, &actual),
	    "%s.%s is not an unsigned integer", name, key);
	expect_u64_eq(actual, expected, "%s.%s has an unexpected value", name,
	    key);
}

static bool
stats_json_print_opts(stats_buf_t *sbuf, const char *opts,
    json_fragment_t *jemalloc) {
	stats_buf_init(sbuf);
	malloc_stats_print(stats_buf_write_cb, sbuf, opts);

	json_fragment_t root;
	if (json_fragment_init(sbuf->buf, sbuf->buf + sbuf->len, &root)
	    || json_object_member(root, "jemalloc", jemalloc)) {
		return true;
	}
	return false;
}

static bool
stats_json_print(stats_buf_t *sbuf, json_fragment_t *stats,
    json_fragment_t *merged) {
	json_fragment_t jemalloc, stats_arenas;
	if (stats_json_print_opts(sbuf, "Ja", &jemalloc)
	    || json_object_member(jemalloc, "stats", stats)
	    || json_object_member(jemalloc, "stats.arenas", &stats_arenas)
	    || json_object_member(stats_arenas, "merged", merged)) {
		return true;
	}
	return false;
}

static const ctl_field_t mutex_fields[] = {
	{"num_ops", "num_ops", ctl_field_uint64},
	{"num_wait", "num_wait", ctl_field_uint64},
	{"num_spin_acq", "num_spin_acq", ctl_field_uint64},
	{"num_owner_switch", "num_owner_switch", ctl_field_uint64},
	{"total_wait_time", "total_wait_time", ctl_field_uint64},
	{"max_wait_time", "max_wait_time", ctl_field_uint64},
	{"max_num_thds", "max_num_thds", ctl_field_uint32},
};
static const char *const mutex_keys[] = {"num_ops", "num_wait",
	"num_spin_acq", "num_owner_switch", "total_wait_time",
	"max_wait_time", "max_num_thds"};

static void
expect_mutex_object(json_fragment_t mutex, const char *ctl_prefix) {
	expect_json_object_keys(
	    mutex, mutex_keys, ARRAY_COUNT(mutex_keys), ctl_prefix);
	expect_json_ctl_fields(
	    mutex, ctl_prefix, mutex_fields, ARRAY_COUNT(mutex_fields));
}

static const ctl_field_t global_fields[] = {
	{"allocated", "allocated", ctl_field_size},
	{"active", "active", ctl_field_size},
	{"metadata", "metadata", ctl_field_size},
	{"metadata_edata", "metadata_edata", ctl_field_size},
	{"metadata_rtree", "metadata_rtree", ctl_field_size},
	{"metadata_thp", "metadata_thp", ctl_field_size},
	{"resident", "resident", ctl_field_size},
	{"mapped", "mapped", ctl_field_size},
	{"retained", "retained", ctl_field_size},
	{"pinned", "pinned", ctl_field_size},
	{"zero_reallocs", "zero_reallocs", ctl_field_size},
};

static const ctl_field_t background_thread_fields[] = {
	{"num_threads", "num_threads", ctl_field_size},
	{"num_runs", "num_runs", ctl_field_uint64},
	{"run_interval", "run_interval", ctl_field_uint64},
};

static const ctl_field_t arena_fields[] = {
	{"nthreads", "nthreads", ctl_field_unsigned},
	{"uptime_ns", "uptime", ctl_field_uint64},
	{"dss", "dss", ctl_field_string},
	{"dirty_decay_ms", "dirty_decay_ms", ctl_field_ssize},
	{"muzzy_decay_ms", "muzzy_decay_ms", ctl_field_ssize},
	{"pactive", "pactive", ctl_field_size},
	{"pdirty", "pdirty", ctl_field_size},
	{"pmuzzy", "pmuzzy", ctl_field_size},
	{"dirty_npurge", "dirty_npurge", ctl_field_uint64},
	{"dirty_nmadvise", "dirty_nmadvise", ctl_field_uint64},
	{"dirty_purged", "dirty_purged", ctl_field_uint64},
	{"muzzy_npurge", "muzzy_npurge", ctl_field_uint64},
	{"muzzy_nmadvise", "muzzy_nmadvise", ctl_field_uint64},
	{"muzzy_purged", "muzzy_purged", ctl_field_uint64},
	{"mapped", "mapped", ctl_field_size},
	{"retained", "retained", ctl_field_size},
	{"pinned", "pinned", ctl_field_size},
	{"base", "base", ctl_field_size},
	{"internal", "internal", ctl_field_size},
	{"metadata_edata", "metadata_edata", ctl_field_size},
	{"metadata_rtree", "metadata_rtree", ctl_field_size},
	{"metadata_thp", "metadata_thp", ctl_field_size},
	{"tcache_bytes", "tcache_bytes", ctl_field_size},
	{"tcache_stashed_bytes", "tcache_stashed_bytes", ctl_field_size},
	{"resident", "resident", ctl_field_size},
	{"abandoned_vm", "abandoned_vm", ctl_field_size},
	{"extent_avail", "extent_avail", ctl_field_size},
};

static const ctl_field_t arena_alloc_fields[] = {
	{"allocated", "allocated", ctl_field_size},
	{"nmalloc", "nmalloc", ctl_field_uint64},
	{"ndalloc", "ndalloc", ctl_field_uint64},
	{"nrequests", "nrequests", ctl_field_uint64},
	{"nfills", "nfills", ctl_field_uint64},
	{"nflushes", "nflushes", ctl_field_uint64},
};
static const char *const arena_alloc_keys[] = {"allocated", "nmalloc",
	"ndalloc", "nrequests", "nfills", "nflushes"};

static const ctl_field_t pac_sec_fields[] = {
	{"pac_sec_bytes", "pac_sec_bytes", ctl_field_size},
	{"pac_sec_hits", "pac_sec_hits", ctl_field_size},
	{"pac_sec_misses", "pac_sec_misses", ctl_field_size},
	{"pac_sec_dalloc_noflush", "pac_sec_dalloc_noflush", ctl_field_size},
	{"pac_sec_dalloc_flush", "pac_sec_dalloc_flush", ctl_field_size},
};

static const char *const merged_keys[] = {"nthreads", "uptime_ns", "dss",
	"dirty_decay_ms", "muzzy_decay_ms", "pactive", "pdirty", "pmuzzy",
	"dirty_npurge", "dirty_nmadvise", "dirty_purged", "muzzy_npurge",
	"muzzy_nmadvise", "muzzy_purged", "small", "large", "mapped",
	"retained", "pinned", "base", "internal", "metadata_edata",
	"metadata_rtree", "metadata_thp", "tcache_bytes",
	"tcache_stashed_bytes", "resident", "abandoned_vm", "extent_avail",
	"mutexes", "bins", "lextents", "extents", "hpa_shard"};
static const char *const pac_sec_keys[] = {"pac_sec_bytes", "pac_sec_hits",
	"pac_sec_misses", "pac_sec_dalloc_noflush", "pac_sec_dalloc_flush"};

static const ctl_field_t arena_sample_fields[] = {
	{"nthreads", "nthreads", ctl_field_unsigned},
	{"dss", "dss", ctl_field_string},
	{"pactive", "pactive", ctl_field_size},
	{"mapped", "mapped", ctl_field_size},
};

static unsigned
create_manual_arena(void) {
	unsigned arena_ind = 0;
	size_t sz = sizeof(arena_ind);
	assert_d_eq(mallctl("arenas.create", &arena_ind, &sz, NULL, 0), 0,
	    "Could not create manual arena");
	return arena_ind;
}

static void
destroy_manual_arena(unsigned arena_ind) {
	size_t mib[3];
	size_t miblen = ARRAY_COUNT(mib);
	assert_d_eq(mallctlnametomib("arena.0.destroy", mib, &miblen), 0,
	    "Could not resolve arena destroy mallctl");
	mib[1] = arena_ind;
	assert_d_eq(mallctlbymib(mib, miblen, NULL, NULL, NULL, 0), 0,
	    "Could not destroy arena %u", arena_ind);
}

static void
expect_arena_sample(json_fragment_t stats_arenas, const char *json_key,
    unsigned ctl_ind, bool has_name) {
	json_fragment_t arena;
	bool missing = json_object_member(stats_arenas, json_key, &arena);
	expect_false(missing, "stats.arenas is missing %s", json_key);
	if (missing) {
		return;
	}

	char ctl_prefix[64];
	malloc_snprintf(ctl_prefix, sizeof(ctl_prefix), "stats.arenas.%u",
	    ctl_ind);
	expect_json_ctl_fields(arena, ctl_prefix, arena_sample_fields,
	    ARRAY_COUNT(arena_sample_fields));

	json_fragment_t name;
	bool name_missing = json_object_member(arena, "name", &name);
	if (has_name) {
		expect_false(name_missing, "stats.arenas.%s is missing name",
		    json_key);
	} else {
		expect_true(name_missing,
		    "stats.arenas.%s unexpectedly contains name", json_key);
	}
}

typedef enum {
	option_parent_jemalloc,
	option_parent_stats,
	option_parent_arenas,
	option_parent_merged,
} option_parent_t;

typedef struct {
	const char     *opts;
	option_parent_t parent;
	const char     *omitted_key;
} option_case_t;

static void
expect_option_omits(const option_case_t *test_case) {
	stats_buf_t sbuf;
	json_fragment_t jemalloc;
	bool malformed = stats_json_print_opts(
	    &sbuf, test_case->opts, &jemalloc);
	expect_false(malformed, "Malformed JSON for opts=%s", test_case->opts);
	if (malformed) {
		stats_buf_fini(&sbuf);
		return;
	}

	json_fragment_t stats, stats_arenas, merged;
	json_fragment_t parent = jemalloc;
	if (test_case->parent >= option_parent_stats) {
		assert_false(json_object_member(jemalloc, "stats", &stats),
		    "opts=%s omitted stats", test_case->opts);
		parent = stats;
	}
	if (test_case->parent >= option_parent_arenas) {
		assert_false(json_object_member(
		                 jemalloc, "stats.arenas", &stats_arenas),
		    "opts=%s omitted stats.arenas", test_case->opts);
		parent = stats_arenas;
	}
	if (test_case->parent >= option_parent_merged) {
		assert_false(json_object_member(stats_arenas, "merged", &merged),
		    "opts=%s omitted merged stats", test_case->opts);
		parent = merged;
	}

	json_fragment_t ignored;
	expect_true(json_object_member(
	                parent, test_case->omitted_key, &ignored),
	    "opts=%s did not omit %s", test_case->opts,
	    test_case->omitted_key);

	if (strcmp(test_case->opts, "Jx") == 0) {
		assert_false(json_object_member(
		                 jemalloc, "stats.arenas", &stats_arenas),
		    "opts=Jx omitted stats.arenas");
		assert_false(json_object_member(stats_arenas, "merged", &merged),
		    "opts=Jx omitted merged stats");
		expect_true(json_object_member(merged, "mutexes", &ignored),
		    "opts=Jx did not omit arena mutexes");
	}
	stats_buf_fini(&sbuf);
}

TEST_BEGIN(test_json_stats_arenas_and_options) {
	test_skip_if(!config_stats);

	unsigned manual_ind = create_manual_arena();
	unsigned destroyed_ind = create_manual_arena();
	void *manual_alloc = mallocx(1,
	    MALLOCX_ARENA(manual_ind) | MALLOCX_TCACHE_NONE);
	assert_ptr_not_null(manual_alloc, "Manual arena allocation failed");
	void *destroyed_alloc = mallocx(1,
	    MALLOCX_ARENA(destroyed_ind) | MALLOCX_TCACHE_NONE);
	assert_ptr_not_null(destroyed_alloc,
	    "Destroyed arena allocation failed");
	dallocx(destroyed_alloc, MALLOCX_TCACHE_NONE);
	destroy_manual_arena(destroyed_ind);

	stats_buf_t sbuf;
	json_fragment_t jemalloc;
	bool malformed = stats_json_print_opts(&sbuf, "J", &jemalloc);
	assert_false(malformed, "Malformed default JSON stats");
	json_fragment_t stats_arenas;
	assert_false(json_object_member(
	                 jemalloc, "stats.arenas", &stats_arenas),
	    "Default JSON stats omitted stats.arenas");

	char manual_key[32];
	malloc_snprintf(
	    manual_key, sizeof(manual_key), "%u", manual_ind);
	expect_arena_sample(stats_arenas, "0", 0, true);
	expect_arena_sample(
	    stats_arenas, manual_key, manual_ind, true);
	expect_arena_sample(stats_arenas, "destroyed",
	    MALLCTL_ARENAS_DESTROYED, false);
	stats_buf_fini(&sbuf);

	const option_case_t cases[] = {
	    {"Jg", option_parent_jemalloc, "version"},
	    {"Jm", option_parent_arenas, "merged"},
	    {"Jd", option_parent_arenas, "destroyed"},
	    {"Ja", option_parent_arenas, manual_key},
	    {"Jb", option_parent_merged, "bins"},
	    {"Jl", option_parent_merged, "lextents"},
	    {"Jx", option_parent_stats, "mutexes"},
	    {"Je", option_parent_merged, "extents"},
	    {"Jh", option_parent_merged, "hpa_shard"},
	    {"Jmda", option_parent_jemalloc, "stats.arenas"},
	};
	for (size_t i = 0; i < ARRAY_COUNT(cases); i++) {
		expect_option_omits(&cases[i]);
	}

	dallocx(manual_alloc, MALLOCX_TCACHE_NONE);
	destroy_manual_arena(manual_ind);
}
TEST_END

TEST_BEGIN(test_json_stats_global_and_arena) {
	test_skip_if(!config_stats);

	stats_buf_t sbuf;
	json_fragment_t stats, merged;
	bool malformed = stats_json_print(&sbuf, &stats, &merged);
	expect_false(malformed, "Could not find runtime stats in JSON output");
	if (malformed) {
		stats_buf_fini(&sbuf);
		return;
	}

	const char *const stats_keys[] = {"allocated", "active", "metadata",
	    "metadata_edata", "metadata_rtree", "metadata_thp", "resident",
	    "mapped", "retained", "pinned", "zero_reallocs",
	    "background_thread", "mutexes"};
	expect_json_object_keys(
	    stats, stats_keys, ARRAY_COUNT(stats_keys), "stats");
	expect_json_ctl_fields(
	    stats, "stats", global_fields, ARRAY_COUNT(global_fields));

	json_fragment_t background_thread;
	expect_false(json_object_member(
	                 stats, "background_thread", &background_thread),
	    "stats is missing background_thread");
	const char *const background_thread_keys[] = {
	    "num_threads", "num_runs", "run_interval"};
	expect_json_object_keys(background_thread, background_thread_keys,
	    ARRAY_COUNT(background_thread_keys), "stats.background_thread");
	if (have_background_thread) {
		expect_json_ctl_fields(background_thread, "stats.background_thread",
		    background_thread_fields,
		    ARRAY_COUNT(background_thread_fields));
	} else {
		for (size_t i = 0; i < ARRAY_COUNT(background_thread_keys); i++) {
			expect_json_uint_eq(background_thread,
			    background_thread_keys[i], 0, "stats.background_thread");
		}
	}

	char arena_prefix[64];
	malloc_snprintf(arena_prefix, sizeof(arena_prefix), "stats.arenas.%u",
	    MALLCTL_ARENAS_ALL);
	size_t expected_merged_keys = ARRAY_COUNT(merged_keys);
	if (opt_pac_sec_opts.nshards > 0) {
		expected_merged_keys += ARRAY_COUNT(pac_sec_keys);
	}
	expect_zu_eq(json_object_size(merged), expected_merged_keys,
	    "merged arena stats has an unexpected number of keys");
	expect_json_object_has_keys(merged, merged_keys, ARRAY_COUNT(merged_keys),
	    "merged arena stats");
	expect_json_ctl_fields(
	    merged, arena_prefix, arena_fields, ARRAY_COUNT(arena_fields));
	if (opt_pac_sec_opts.nshards > 0) {
		expect_json_object_has_keys(merged, pac_sec_keys,
		    ARRAY_COUNT(pac_sec_keys), "merged arena stats");
		expect_json_ctl_fields(merged, arena_prefix, pac_sec_fields,
		    ARRAY_COUNT(pac_sec_fields));
	}

	json_fragment_t small, large;
	expect_false(json_object_member(merged, "small", &small),
	    "merged arena stats are missing small");
	expect_false(json_object_member(merged, "large", &large),
	    "merged arena stats are missing large");
	expect_json_object_keys(small, arena_alloc_keys,
	    ARRAY_COUNT(arena_alloc_keys), "stats.arenas.merged.small");
	expect_json_object_keys(large, arena_alloc_keys,
	    ARRAY_COUNT(arena_alloc_keys), "stats.arenas.merged.large");
	char alloc_prefix[96];
	malloc_snprintf(
	    alloc_prefix, sizeof(alloc_prefix), "%s.small", arena_prefix);
	expect_json_ctl_fields(small, alloc_prefix, arena_alloc_fields,
	    ARRAY_COUNT(arena_alloc_fields));
	malloc_snprintf(
	    alloc_prefix, sizeof(alloc_prefix), "%s.large", arena_prefix);
	expect_json_ctl_fields(large, alloc_prefix, arena_alloc_fields,
	    ARRAY_COUNT(arena_alloc_fields));

	json_fragment_t mutexes;
	expect_false(json_object_member(stats, "mutexes", &mutexes),
	    "stats is missing mutexes");
	expect_zu_eq(json_object_size(mutexes), num_global_mutexes,
	    "stats.mutexes has an unexpected number of mutexes");
	for (size_t i = 0; i < num_global_mutexes; i++) {
		json_fragment_t mutex;
		expect_false(json_object_member(
		                 mutexes, global_mutex_names[i], &mutex),
		    "stats.mutexes is missing %s", global_mutex_names[i]);
		char prefix[128];
		malloc_snprintf(prefix, sizeof(prefix), "stats.mutexes.%s",
		    global_mutex_names[i]);
		expect_mutex_object(mutex, prefix);
	}

	expect_false(json_object_member(merged, "mutexes", &mutexes),
	    "merged arena stats are missing mutexes");
	expect_zu_eq(json_object_size(mutexes), num_arena_mutexes,
	    "merged arena mutexes has an unexpected number of mutexes");
	for (size_t i = 0; i < num_arena_mutexes; i++) {
		json_fragment_t mutex;
		expect_false(json_object_member(
		                 mutexes, arena_mutex_names[i], &mutex),
		    "merged arena mutexes is missing %s", arena_mutex_names[i]);
		char prefix[160];
		malloc_snprintf(prefix, sizeof(prefix), "%s.mutexes.%s",
		    arena_prefix, arena_mutex_names[i]);
		expect_mutex_object(mutex, prefix);
	}

	stats_buf_fini(&sbuf);
}
TEST_END

static const ctl_field_t hpa_fields[] = {
	{"npageslabs", "npageslabs", ctl_field_size},
	{"nactive", "nactive", ctl_field_size},
	{"ndirty", "ndirty", ctl_field_size},
	{"npurge_passes", "npurge_passes", ctl_field_uint64},
	{"npurges", "npurges", ctl_field_uint64},
	{"nhugifies", "nhugifies", ctl_field_uint64},
	{"nhugify_failures", "nhugify_failures", ctl_field_uint64},
	{"ndehugifies", "ndehugifies", ctl_field_uint64},
};

static const ctl_field_t hpa_sec_fields[] = {
	{"sec_bytes", "hpa_sec_bytes", ctl_field_size},
	{"sec_hits", "hpa_sec_hits", ctl_field_size},
	{"sec_misses", "hpa_sec_misses", ctl_field_size},
	{"sec_dalloc_noflush", "hpa_sec_dalloc_noflush", ctl_field_size},
	{"sec_dalloc_flush", "hpa_sec_dalloc_flush", ctl_field_size},
	{"sec_overfills", "hpa_sec_overfills", ctl_field_size},
};

static const ctl_field_t hpa_slab_fields[] = {
	{"npageslabs_huge", "npageslabs_huge", ctl_field_size},
	{"nactive_huge", "nactive_huge", ctl_field_size},
	{"ndirty_huge", "ndirty_huge", ctl_field_size},
	{"npageslabs_nonhuge", "npageslabs_nonhuge", ctl_field_size},
	{"nactive_nonhuge", "nactive_nonhuge", ctl_field_size},
	{"ndirty_nonhuge", "ndirty_nonhuge", ctl_field_size},
};
static const char *const hpa_slab_keys[] = {"npageslabs_huge",
	"nactive_huge", "ndirty_huge", "npageslabs_nonhuge",
	"nactive_nonhuge", "ndirty_nonhuge", "nretained_nonhuge"};

static void
expect_hpa_slab(json_fragment_t slab, const char *ctl_prefix,
    const char *name) {
	expect_json_object_keys(
	    slab, hpa_slab_keys, ARRAY_COUNT(hpa_slab_keys), name);
	expect_json_ctl_fields(slab, ctl_prefix, hpa_slab_fields,
	    ARRAY_COUNT(hpa_slab_fields));

	size_t npageslabs, nactive, ndirty;
	size_t sz = sizeof(size_t);
	char ctl_name[192];
	malloc_snprintf(ctl_name, sizeof(ctl_name),
	    "%s.npageslabs_nonhuge", ctl_prefix);
	expect_d_eq(mallctl(ctl_name, &npageslabs, &sz, NULL, 0), 0,
	    "mallctl failed for %s", ctl_name);
	sz = sizeof(size_t);
	malloc_snprintf(
	    ctl_name, sizeof(ctl_name), "%s.nactive_nonhuge", ctl_prefix);
	expect_d_eq(mallctl(ctl_name, &nactive, &sz, NULL, 0), 0,
	    "mallctl failed for %s", ctl_name);
	sz = sizeof(size_t);
	malloc_snprintf(
	    ctl_name, sizeof(ctl_name), "%s.ndirty_nonhuge", ctl_prefix);
	expect_d_eq(mallctl(ctl_name, &ndirty, &sz, NULL, 0), 0,
	    "mallctl failed for %s", ctl_name);
	expect_json_uint_eq(slab, "nretained_nonhuge",
	    npageslabs * HUGEPAGE_PAGES - nactive - ndirty, name);
}

static const ctl_field_t hpa_alloc_fields[] = {
	{"min_extents", "min_extents", ctl_field_uint64},
	{"max_extents", "max_extents", ctl_field_uint64},
	{"extents", "extents", ctl_field_uint64},
	{"ps", "ps", ctl_field_uint64},
	{"pages_per_ps", "pages_per_ps", ctl_field_uint64},
	{"extents_per_ps", "extents_per_ps", ctl_field_uint64},
	{"total_elapsed_ns_per_ps", "total_elapsed_ns_per_ps",
	    ctl_field_uint64},
};
static const char *const hpa_alloc_keys[] = {"min_extents", "max_extents",
	"extents", "ps", "pages_per_ps", "extents_per_ps",
	"total_elapsed_ns_per_ps"};

TEST_BEGIN(test_json_stats_hpa) {
	test_skip_if(!config_stats);
	test_skip_if(!hpa_supported());

	stats_buf_t sbuf;
	json_fragment_t stats, merged;
	bool malformed = stats_json_print(&sbuf, &stats, &merged);
	expect_false(malformed, "Could not find runtime stats in JSON output");
	if (malformed) {
		stats_buf_fini(&sbuf);
		return;
	}

	json_fragment_t hpa;
	expect_false(json_object_member(merged, "hpa_shard", &hpa),
	    "merged arena stats are missing hpa_shard");
	const char *const hpa_keys[] = {"sec_bytes", "sec_hits", "sec_misses",
	    "sec_dalloc_noflush", "sec_dalloc_flush", "sec_overfills",
	    "npageslabs", "nactive", "ndirty", "npurge_passes", "npurges",
	    "nhugifies", "nhugify_failures", "ndehugifies", "slabs",
	    "extent_allocation_distribution", "full_slabs", "empty_slabs",
	    "nonfull_slabs"};
	expect_json_object_keys(
	    hpa, hpa_keys, ARRAY_COUNT(hpa_keys), "hpa_shard");

	char arena_prefix[64];
	malloc_snprintf(arena_prefix, sizeof(arena_prefix), "stats.arenas.%u",
	    MALLCTL_ARENAS_ALL);
	expect_json_ctl_fields(hpa, arena_prefix, hpa_sec_fields,
	    ARRAY_COUNT(hpa_sec_fields));
	char hpa_prefix[96];
	malloc_snprintf(
	    hpa_prefix, sizeof(hpa_prefix), "%s.hpa_shard", arena_prefix);
	expect_json_ctl_fields(
	    hpa, hpa_prefix, hpa_fields, ARRAY_COUNT(hpa_fields));

	json_fragment_t slabs;
	expect_false(json_object_member(hpa, "slabs", &slabs),
	    "hpa_shard is missing slabs");
	char slab_prefix[128];
	malloc_snprintf(
	    slab_prefix, sizeof(slab_prefix), "%s.slabs", hpa_prefix);
	expect_hpa_slab(slabs, slab_prefix, "hpa_shard.slabs");

	const char *const summary_names[] = {"full_slabs", "empty_slabs"};
	for (size_t i = 0; i < ARRAY_COUNT(summary_names); i++) {
		json_fragment_t summary;
		expect_false(json_object_member(hpa, summary_names[i], &summary),
		    "hpa_shard is missing %s", summary_names[i]);
		malloc_snprintf(slab_prefix, sizeof(slab_prefix), "%s.%s",
		    hpa_prefix, summary_names[i]);
		expect_hpa_slab(summary, slab_prefix, summary_names[i]);
	}

	json_fragment_t nonfull;
	expect_false(json_object_member(hpa, "nonfull_slabs", &nonfull),
	    "hpa_shard is missing nonfull_slabs");
	size_t nonfull_count = PSSET_NPSIZES < SC_NPSIZES ? PSSET_NPSIZES
	                                                  : SC_NPSIZES;
	expect_zu_eq(json_array_size(nonfull), nonfull_count,
	    "nonfull_slabs has an unexpected number of rows");
	for (size_t i = 0; i < nonfull_count; i++) {
		json_fragment_t slab;
		expect_false(json_array_element(nonfull, i, &slab),
		    "nonfull_slabs is missing row %zu", i);
		malloc_snprintf(slab_prefix, sizeof(slab_prefix),
		    "%s.nonfull_slabs.%zu", hpa_prefix, i);
		char row_name[64];
		malloc_snprintf(
		    row_name, sizeof(row_name), "nonfull_slabs[%zu]", i);
		expect_hpa_slab(slab, slab_prefix, row_name);
	}

	json_fragment_t distribution;
	expect_false(json_object_member(
	                 hpa, "extent_allocation_distribution", &distribution),
	    "hpa_shard is missing extent_allocation_distribution");
	expect_zu_eq(json_array_size(distribution), SEC_MAX_NALLOCS + 1,
	    "extent_allocation_distribution has an unexpected row count");
	for (size_t i = 0; i <= SEC_MAX_NALLOCS; i++) {
		json_fragment_t row;
		expect_false(json_array_element(distribution, i, &row),
		    "extent_allocation_distribution is missing row %zu", i);
		char row_name[80];
		malloc_snprintf(row_name, sizeof(row_name),
		    "extent_allocation_distribution[%zu]", i);
		expect_json_object_keys(row, hpa_alloc_keys,
		    ARRAY_COUNT(hpa_alloc_keys), row_name);
		char ctl_prefix[128];
		malloc_snprintf(ctl_prefix, sizeof(ctl_prefix),
		    "%s.alloc.%zu", hpa_prefix, i);
		expect_json_ctl_fields(row, ctl_prefix, hpa_alloc_fields,
		    ARRAY_COUNT(hpa_alloc_fields));
	}

	stats_buf_fini(&sbuf);
}
TEST_END

static const ctl_field_t bin_fields[] = {
	{"nmalloc", "nmalloc", ctl_field_uint64},
	{"ndalloc", "ndalloc", ctl_field_uint64},
	{"curregs", "curregs", ctl_field_size},
	{"nrequests", "nrequests", ctl_field_uint64},
	{"nfills", "nfills", ctl_field_uint64},
	{"nflushes", "nflushes", ctl_field_uint64},
	{"nreslabs", "nreslabs", ctl_field_uint64},
	{"curslabs", "curslabs", ctl_field_size},
	{"nonfull_slabs", "nonfull_slabs", ctl_field_size},
};
static const char *const bin_keys[] = {"nmalloc", "ndalloc", "curregs",
	"nrequests", "nfills", "nflushes", "nreslabs", "curslabs",
	"nonfull_slabs", "mutex"};

static const ctl_field_t lextent_fields[] = {
	{"curlextents", "curlextents", ctl_field_size},
};
static const char *const lextent_keys[] = {"curlextents"};

static const ctl_field_t extent_fields[] = {
	{"ndirty", "ndirty", ctl_field_size},
	{"nmuzzy", "nmuzzy", ctl_field_size},
	{"nretained", "nretained", ctl_field_size},
	{"npinned", "npinned", ctl_field_size},
	{"dirty_bytes", "dirty_bytes", ctl_field_size},
	{"muzzy_bytes", "muzzy_bytes", ctl_field_size},
	{"retained_bytes", "retained_bytes", ctl_field_size},
	{"pinned_bytes", "pinned_bytes", ctl_field_size},
};
static const char *const extent_keys[] = {"ndirty", "nmuzzy", "nretained",
	"npinned", "dirty_bytes", "muzzy_bytes", "retained_bytes",
	"pinned_bytes"};

static const char *const prof_keys[] = {"prof_live_requested",
	"prof_live_count", "prof_accum_requested", "prof_accum_count"};

static void
expect_prof_stats(json_fragment_t object, const char *ctl_prefix) {
	prof_stats_t live, accum;
	size_t sz = sizeof(prof_stats_t);
	char ctl_name[128];
	malloc_snprintf(
	    ctl_name, sizeof(ctl_name), "%s.live", ctl_prefix);
	expect_d_eq(mallctl(ctl_name, &live, &sz, NULL, 0), 0,
	    "mallctl failed for %s", ctl_name);
	sz = sizeof(prof_stats_t);
	malloc_snprintf(
	    ctl_name, sizeof(ctl_name), "%s.accum", ctl_prefix);
	expect_d_eq(mallctl(ctl_name, &accum, &sz, NULL, 0), 0,
	    "mallctl failed for %s", ctl_name);
	expect_json_uint_eq(object, "prof_live_requested", live.req_sum,
	    ctl_prefix);
	expect_json_uint_eq(
	    object, "prof_live_count", live.count, ctl_prefix);
	expect_json_uint_eq(object, "prof_accum_requested", accum.req_sum,
	    ctl_prefix);
	expect_json_uint_eq(
	    object, "prof_accum_count", accum.count, ctl_prefix);
}

TEST_BEGIN(test_json_stats_arena_tables) {
	test_skip_if(!config_stats);

	stats_buf_t sbuf;
	json_fragment_t stats, merged;
	bool malformed = stats_json_print(&sbuf, &stats, &merged);
	expect_false(malformed, "Could not find runtime stats in JSON output");
	if (malformed) {
		stats_buf_fini(&sbuf);
		return;
	}

	unsigned nbins, nlextents;
	size_t sz = sizeof(nbins);
	expect_d_eq(mallctl("arenas.nbins", &nbins, &sz, NULL, 0), 0,
	    "mallctl failed for arenas.nbins");
	sz = sizeof(nlextents);
	expect_d_eq(mallctl("arenas.nlextents", &nlextents, &sz, NULL, 0), 0,
	    "mallctl failed for arenas.nlextents");

	json_fragment_t bins;
	expect_false(json_object_member(merged, "bins", &bins),
	    "merged arena stats are missing bins");
	expect_zu_eq(json_array_size(bins), nbins,
	    "bins has an unexpected number of rows");
	json_fragment_t first_bin, ignored;
	expect_false(json_array_element(bins, 0, &first_bin),
	    "bins is missing its first row");
	bool prof_stats_present = !json_object_member(
	    first_bin, "prof_live_requested", &ignored);
	for (unsigned i = 0; i < nbins; i++) {
		json_fragment_t bin;
		expect_false(json_array_element(bins, i, &bin),
		    "bins is missing row %u", i);
		char row_name[64];
		malloc_snprintf(row_name, sizeof(row_name), "bins[%u]", i);
		expect_zu_eq(json_object_size(bin), ARRAY_COUNT(bin_keys)
		        + (prof_stats_present ? ARRAY_COUNT(prof_keys) : 0),
		    "%s has an unexpected number of keys", row_name);
		expect_json_object_has_keys(
		    bin, bin_keys, ARRAY_COUNT(bin_keys), row_name);
		char ctl_prefix[128];
		malloc_snprintf(ctl_prefix, sizeof(ctl_prefix),
		    "stats.arenas.%u.bins.%u", MALLCTL_ARENAS_ALL, i);
		expect_json_ctl_fields(
		    bin, ctl_prefix, bin_fields, ARRAY_COUNT(bin_fields));
		if (prof_stats_present) {
			expect_json_object_has_keys(
			    bin, prof_keys, ARRAY_COUNT(prof_keys), row_name);
			char prof_prefix[128];
			malloc_snprintf(prof_prefix, sizeof(prof_prefix),
			    "prof.stats.bins.%u", i);
			expect_prof_stats(bin, prof_prefix);
		}

		json_fragment_t mutex;
		expect_false(json_object_member(bin, "mutex", &mutex),
		    "%s is missing mutex", row_name);
		char mutex_prefix[160];
		malloc_snprintf(mutex_prefix, sizeof(mutex_prefix), "%s.mutex",
		    ctl_prefix);
		expect_mutex_object(mutex, mutex_prefix);
	}

	json_fragment_t lextents;
	expect_false(json_object_member(merged, "lextents", &lextents),
	    "merged arena stats are missing lextents");
	expect_zu_eq(json_array_size(lextents), nlextents,
	    "lextents has an unexpected number of rows");
	for (unsigned i = 0; i < nlextents; i++) {
		json_fragment_t lextent;
		expect_false(json_array_element(lextents, i, &lextent),
		    "lextents is missing row %u", i);
		char row_name[64];
		malloc_snprintf(row_name, sizeof(row_name), "lextents[%u]", i);
		expect_zu_eq(json_object_size(lextent), ARRAY_COUNT(lextent_keys)
		        + (prof_stats_present ? ARRAY_COUNT(prof_keys) : 0),
		    "%s has an unexpected number of keys", row_name);
		expect_json_object_has_keys(lextent, lextent_keys,
		    ARRAY_COUNT(lextent_keys), row_name);
		char ctl_prefix[128];
		malloc_snprintf(ctl_prefix, sizeof(ctl_prefix),
		    "stats.arenas.%u.lextents.%u", MALLCTL_ARENAS_ALL, i);
		expect_json_ctl_fields(lextent, ctl_prefix, lextent_fields,
		    ARRAY_COUNT(lextent_fields));
		if (prof_stats_present) {
			expect_json_object_has_keys(lextent, prof_keys,
			    ARRAY_COUNT(prof_keys), row_name);
			char prof_prefix[128];
			malloc_snprintf(prof_prefix, sizeof(prof_prefix),
			    "prof.stats.lextents.%u", i);
			expect_prof_stats(lextent, prof_prefix);
		}
	}

	json_fragment_t extents;
	expect_false(json_object_member(merged, "extents", &extents),
	    "merged arena stats are missing extents");
	expect_zu_eq(json_array_size(extents), SC_NPSIZES,
	    "extents has an unexpected number of rows");
	for (unsigned i = 0; i < SC_NPSIZES; i++) {
		json_fragment_t extent;
		expect_false(json_array_element(extents, i, &extent),
		    "extents is missing row %u", i);
		char row_name[64];
		malloc_snprintf(row_name, sizeof(row_name), "extents[%u]", i);
		expect_json_object_keys(
		    extent, extent_keys, ARRAY_COUNT(extent_keys), row_name);
		char ctl_prefix[128];
		malloc_snprintf(ctl_prefix, sizeof(ctl_prefix),
		    "stats.arenas.%u.extents.%u", MALLCTL_ARENAS_ALL, i);
		expect_json_ctl_fields(extent, ctl_prefix, extent_fields,
		    ARRAY_COUNT(extent_fields));
	}

	stats_buf_fini(&sbuf);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(test_json_stats_global_and_arena,
	    test_json_stats_arena_tables, test_json_stats_hpa,
	    test_json_stats_arenas_and_options);
}
