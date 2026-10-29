#include "test/jemalloc_test.h"

#include "jemalloc/internal/conf.h"
#include "jemalloc/internal/hpa_shard_set.h"

/*
 * The MALLOC_CONF in hpa_shard_set_conf.sh turns on shard sets and sets
 * hpa_shard_sets_nshards twice; the second occurrence must replace the first.
 */
TEST_BEGIN(test_options_parsed) {
	expect_true(opt_hpa_shard_sets, "hpa_shard_sets not parsed");

	unsigned expected[HPA_SHARD_SET_NBANDS] = {4, 4, 0, 2};
	for (unsigned i = 0; i < HPA_SHARD_SET_NBANDS; i++) {
		expect_u_eq(opt_hpa_shard_sets_nshards.nshards[i], expected[i],
		    "hpa_shard_sets_nshards band %u", i);
	}
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
	return conf_parse_hpa_shard_sets_nshards(value, strlen(value), config);
}

static void
expect_nshards(const hpa_shard_sets_config_t *config, const char *value,
    const unsigned *expected) {
	for (unsigned i = 0; i < HPA_SHARD_SET_NBANDS; i++) {
		expect_u_eq(config->nshards[i], expected[i], "\"%s\" band %u",
		    value, i);
	}
}

TEST_BEGIN(test_parse_valid) {
	struct {
		const char *value;
		unsigned    nshards[HPA_SHARD_SET_NBANDS];
	} cases[] = {
	    /* Bands without a count join the set of the band below. */
	    {"4", {4}},
	    {"4|4|0|2", {4, 4, 0, 2}},
	    {"1|2|3|4|5|6|7|8|9", {1, 2, 3, 4, 5, 6, 7, 8, 9}},
	    {"1|0|0|0|0|0|0|0|0", {1}},
	};
	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		hpa_shard_sets_config_t config;
		expect_false(parse(cases[c].value, &config), "\"%s\" rejected",
		    cases[c].value);
		expect_nshards(&config, cases[c].value, cases[c].nshards);
	}

	/* A band may have as many shards as a set has slots. */
	char                    buf[64];
	hpa_shard_sets_config_t config;
	malloc_snprintf(
	    buf, sizeof(buf), "1|%u", (unsigned)HPA_MAX_SHARDS_PER_SET);
	expect_false(parse(buf, &config), "\"%s\" rejected", buf);
	expect_u_eq(config.nshards[1], HPA_MAX_SHARDS_PER_SET, "");
}
TEST_END

static void
expect_malformed(const char *value) {
	hpa_shard_sets_config_t config;
	unsigned                none[HPA_SHARD_SET_NBANDS] = {0};
	/* A layout from an earlier parse must not survive a malformed one. */
	expect_false(parse("4", &config), "");
	expect_true(parse(value, &config), "\"%s\" accepted", value);
	expect_nshards(&config, value, none);
}

TEST_BEGIN(test_parse_malformed) {
	const char *cases[] = {
	    /* Syntax. */
	    "", "|", "a", "4x", "4|", "|4", "4||2", "4|a",
	    /* The old grammars: ranges and pickers. */
	    "1-16384:4", "4;arena",
	    /* The first band has no band below it to join. */
	    "0", "0|4",
	};
	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		expect_malformed(cases[c]);
	}

	char buf[64];
	malloc_snprintf(
	    buf, sizeof(buf), "1|%u", (unsigned)HPA_MAX_SHARDS_PER_SET + 1);
	expect_malformed(buf);

	/* One count past the last band. */
	size_t len = 0;
	for (unsigned i = 0; i <= HPA_SHARD_SET_NBANDS; i++) {
		len += malloc_snprintf(
		    buf + len, sizeof(buf) - len, "%s1", i == 0 ? "" : "|");
	}
	expect_malformed(buf);
}
TEST_END

TEST_BEGIN(test_parse_replaces_layout) {
	hpa_shard_sets_config_t config;
	unsigned                expected[HPA_SHARD_SET_NBANDS] = {8};
	expect_false(parse("4|4|2|2", &config), "");
	/* A parse replaces the earlier layout rather than merging with it. */
	expect_false(parse("8", &config), "");
	expect_nshards(&config, "8", expected);
}
TEST_END

int
main(void) {
	return test(test_options_parsed, test_options_readback,
	    test_parse_valid, test_parse_malformed,
	    test_parse_replaces_layout);
}
