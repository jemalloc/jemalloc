#include "test/jemalloc_test.h"

#include "jemalloc/internal/conf.h"
#include "jemalloc/internal/hpa_shard_set.h"

/*
 * The MALLOC_CONF in hpa_shard_set_conf.sh turns on shard sets and sets
 * hpa_shard_sets_configs twice; the second occurrence, with three sets (the
 * default picker, an explicit roundrobin, and an explicit arena), must replace
 * the first.
 */
TEST_BEGIN(test_options_parsed) {
	expect_true(opt_hpa_shard_sets, "hpa_shard_sets not parsed");

	hpa_shard_sets_config_t *config = &opt_hpa_shard_sets_configs;
	expect_u_eq(config->nsets, 3, "hpa_shard_sets_configs not parsed");
	expect_zu_eq(config->sets[0].size_max, 16384, "");
	expect_u_eq(config->sets[0].nshards, 4, "");
	expect_u_eq(config->sets[0].pick, hpa_shard_set_pick_arena,
	    "a set without a picker should use arena");
	expect_zu_eq(config->sets[1].size_max, 65536, "");
	expect_u_eq(config->sets[1].nshards, 8, "");
	expect_u_eq(config->sets[1].pick, hpa_shard_set_pick_roundrobin, "");
	expect_zu_eq(config->sets[2].size_max, 2097152, "");
	expect_u_eq(config->sets[2].nshards, 2, "");
	expect_u_eq(config->sets[2].pick, hpa_shard_set_pick_arena, "");
}
TEST_END

TEST_BEGIN(test_options_readback) {
	bool   enabled;
	size_t sz = sizeof(enabled);
	expect_d_eq(mallctl("opt.hpa_shard_sets", &enabled, &sz, NULL, 0), 0,
	    "");
	expect_true(enabled, "");
}
TEST_END

static bool
parse(const char *value, hpa_shard_sets_config_t *config) {
	return conf_parse_hpa_shard_sets_configs(value, strlen(value), config);
}

TEST_BEGIN(test_parse_valid) {
	struct {
		const char          *value;
		unsigned             nsets;
		size_t               size_max[3];
		unsigned             nshards[3];
		hpa_shard_set_pick_t pick[3];
	} cases[] = {
	    {"1-2097152:4", 1, {2097152}, {4}, {hpa_shard_set_pick_arena}},
	    {"1-2097152:4;roundrobin", 1, {2097152}, {4},
	        {hpa_shard_set_pick_roundrobin}},
	    {"1-16384:4|16385-65536:8;roundrobin|65537-2097152:2;arena", 3,
	        {16384, 65536, 2097152}, {4, 8, 2},
	        {hpa_shard_set_pick_arena, hpa_shard_set_pick_roundrobin,
	            hpa_shard_set_pick_arena}},
	};
	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		hpa_shard_sets_config_t config;
		expect_false(parse(cases[c].value, &config), "\"%s\" rejected",
		    cases[c].value);
		expect_u_eq(config.nsets, cases[c].nsets, "\"%s\"",
		    cases[c].value);
		for (unsigned i = 0; i < cases[c].nsets; i++) {
			expect_zu_eq(config.sets[i].size_max,
			    cases[c].size_max[i], "\"%s\" set %u",
			    cases[c].value, i);
			expect_u_eq(config.sets[i].nshards, cases[c].nshards[i],
			    "\"%s\" set %u", cases[c].value, i);
			expect_u_eq(config.sets[i].pick, cases[c].pick[i],
			    "\"%s\" set %u", cases[c].value, i);
		}
	}
}
TEST_END

static void
expect_malformed(const char *value) {
	hpa_shard_sets_config_t config;
	/* A layout from an earlier parse must not survive a malformed one. */
	expect_false(parse("1-2097152:4", &config), "");
	expect_true(parse(value, &config), "\"%s\" accepted", value);
	expect_u_eq(config.nsets, 0, "\"%s\" left a layout behind", value);
}

TEST_BEGIN(test_parse_malformed) {
	const char *cases[] = {
	    /* Syntax. */
	    "", "1-16384", "1:16384:4", "a-16384:4", "1-16384:4x",
	    /* Shard counts. */
	    "1-16384:0",
	    /* Ranges: must start at 1, be contiguous, in order, non-empty. */
	    "2-16384:4", "1-16384:4|16390-2097152:2",
	    "1-16384:4|8000-2097152:2", "1-1800:4|8-60:2",
	    "1-16384:4|16385-100:1",
	    /* Pickers. */
	    "1-16384:4;bogus", "1-16384:4;round", "1-16384:4;",
	    "1-16384:4;arena;arena",
	    /* A malformed segment after valid ones rejects the whole value. */
	    "1-16384:4|16385-65536:8;roundrobin|65537-2097152:2;bogus",
	};
	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		expect_malformed(cases[c]);
	}

	char buf[256];
	malloc_snprintf(buf, sizeof(buf), "1-2097152:%u",
	    (unsigned)HPA_MAX_SHARDS_PER_SET + 1);
	expect_malformed(buf);

	/* One set past HPA_MAX_SHARD_SETS. */
	size_t len = 0;
	for (unsigned i = 0; i <= HPA_MAX_SHARD_SETS; i++) {
		len += malloc_snprintf(buf + len, sizeof(buf) - len,
		    "%s%u-%u:1", i == 0 ? "" : "|", i * 4096 + 1,
		    (i + 1) * 4096);
	}
	expect_malformed(buf);
}
TEST_END

TEST_BEGIN(test_parse_replaces_layout) {
	hpa_shard_sets_config_t config;
	expect_false(parse("1-16384:4|16385-2097152:2", &config), "");
	expect_false(parse("1-2097152:8;roundrobin", &config), "");
	expect_u_eq(config.nsets, 1,
	    "a parse should replace the earlier layout, not merge with it");
	expect_zu_eq(config.sets[0].size_max, 2097152, "");
	expect_u_eq(config.sets[0].nshards, 8, "");
	expect_u_eq(config.sets[0].pick, hpa_shard_set_pick_roundrobin, "");
}
TEST_END

int
main(void) {
	return test(test_options_parsed, test_options_readback,
	    test_parse_valid, test_parse_malformed,
	    test_parse_replaces_layout);
}
