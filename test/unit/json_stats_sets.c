#include "test/jemalloc_test.h"

/*
 * json_stats, with the HPA shard sets on (see the .sh) and the HPA, which
 * main() turns on where it is supported.
 */
#define JSON_STATS_WITH_HPA
#include "json_stats.c"
