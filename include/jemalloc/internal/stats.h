#ifndef JEMALLOC_INTERNAL_STATS_H
#define JEMALLOC_INTERNAL_STATS_H

/*  OPTION(opt,		var_name,	default,	set_value_to) */
#define STATS_PRINT_OPTIONS						\
    OPTION('J',		json,		false,		true)		\
    OPTION('g',		general,	true,		false)		\
    OPTION('m',		merged,		config_stats,	false)		\
    OPTION('d',		destroyed,	config_stats,	false)		\
    OPTION('a',		unmerged,	config_stats,	false)		\
    OPTION('b',		bins,		true,		false)		\
    OPTION('l',		large,		true,		false)		\
    OPTION('x',		mutex,		true,		false)		\
    OPTION('e',		extents,	true,		false)

enum {
#define OPTION(o, v, d, s) stats_print_option_num_##v,
    STATS_PRINT_OPTIONS
#undef OPTION
    stats_print_tot_num_options
};

/* Options for stats_print. */
extern bool opt_stats_print;
extern char opt_stats_print_opts[stats_print_tot_num_options+1];

/* Utilities for stats_interval. */
extern int64_t opt_stats_interval;
extern char opt_stats_interval_opts[stats_print_tot_num_options+1];

#define STATS_INTERVAL_DEFAULT -1
/*
 * Batch-increment the counter to reduce synchronization overhead.  Each thread
 * merges after (interval >> LG_BATCH_SIZE) bytes of allocations; also limit the
 * BATCH_MAX for accuracy when the interval is huge (which is expected).
 */
#define STATS_INTERVAL_ACCUM_LG_BATCH_SIZE 6
#define STATS_INTERVAL_ACCUM_BATCH_MAX (4 << 20)

uint64_t stats_interval_accum_batch_size(void);
bool stats_interval_accum(tsd_t *tsd, uint64_t bytes);

/* Implements je_malloc_stats_print. */
void stats_print(write_cb_t *write_cb, void *cbopaque, const char *opts);

bool stats_boot(void);

#endif /* JEMALLOC_INTERNAL_STATS_H */
