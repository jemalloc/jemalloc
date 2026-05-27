#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/ctl_mallctl.h"
#include "jemalloc/internal/prof.h"
#include "jemalloc/internal/prof_data.h"
#include "jemalloc/internal/prof_log.h"
#include "jemalloc/internal/prof_recent.h"
#include "jemalloc/internal/prof_stats.h"
#include "jemalloc/internal/prof_sys.h"
#include "jemalloc/internal/sc.h"

/******************************************************************************/
/* prof.* mallctl handlers. */

int
prof_thread_active_init_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	if (!config_prof) {
		return ENOENT;
	}

	bool oldval = false;
	int  ret = 0;
	if (newp != NULL) {
		if (!opt_prof) {
			ret = ENOENT;
		} else {
			bool newval;
			ret = ctl_write(&newval, sizeof(newval), newp, newlen);
			if (ret == 0) {
				oldval = prof_thread_active_init_set(
				    tsd_tsdn(tsd), newval);
			}
		}
	} else {
		oldval = opt_prof ? prof_thread_active_init_get(tsd_tsdn(tsd))
		                  : false;
	}
	if (ret == 0) {
		ret = ctl_read(oldp, oldlenp, &oldval, sizeof(oldval));
	}
	return ret;
}

int
prof_active_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	if (!config_prof) {
		return ENOENT;
	}

	bool oldval = false;
	int  ret = 0;
	if (newp != NULL) {
		bool val;
		ret = ctl_write(&val, sizeof(val), newp, newlen);
		if (ret == 0) {
			if (!opt_prof) {
				if (val) {
					ret = ENOENT;
				} else {
					/* No change needed (already off). */
					oldval = false;
				}
			} else {
				oldval = prof_active_set(tsd_tsdn(tsd), val);
			}
		}
	} else {
		oldval = opt_prof ? prof_active_get(tsd_tsdn(tsd)) : false;
	}
	if (ret == 0) {
		ret = ctl_read(oldp, oldlenp, &oldval, sizeof(oldval));
	}
	return ret;
}

int
prof_dump_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	const char *filename = NULL;

	if (!config_prof || !opt_prof) {
		return ENOENT;
	}

	int ret = ctl_writeonly(oldp, oldlenp);
	if (ret != 0) {
		return ret;
	}

	ret = ctl_write(&filename, sizeof(filename), newp, newlen);
	if (ret != 0) {
		return ret;
	}

	return prof_mdump(tsd, filename) ? EFAULT : 0;
}

int
prof_gdump_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	if (!config_prof) {
		return ENOENT;
	}

	bool oldval = false;
	int  ret = 0;
	if (newp != NULL) {
		if (!opt_prof) {
			ret = ENOENT;
		} else {
			bool newval;
			ret = ctl_write(&newval, sizeof(newval), newp, newlen);
			if (ret == 0) {
				oldval = prof_gdump_set(tsd_tsdn(tsd), newval);
			}
		}
	} else {
		oldval = opt_prof ? prof_gdump_get(tsd_tsdn(tsd)) : false;
	}
	if (ret == 0) {
		ret = ctl_read(oldp, oldlenp, &oldval, sizeof(oldval));
	}
	return ret;
}

int
prof_prefix_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	const char *prefix = NULL;

	if (!config_prof || !opt_prof) {
		return ENOENT;
	}

	ctl_mtx_lock(tsd_tsdn(tsd));
	int ret = ctl_writeonly(oldp, oldlenp);
	if (ret == 0) {
		ret = ctl_write(&prefix, sizeof(prefix), newp, newlen);
	}
	if (ret == 0) {
		ret = prof_prefix_set(tsd_tsdn(tsd), prefix) ? EFAULT : 0;
	}

	ctl_mtx_unlock(tsd_tsdn(tsd));
	return ret;
}

int
prof_reset_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	size_t lg_sample = lg_prof_sample;

	if (!config_prof || !opt_prof) {
		return ENOENT;
	}

	int ret = ctl_writeonly(oldp, oldlenp);
	if (ret != 0) {
		return ret;
	}

	ret = ctl_write(&lg_sample, sizeof(lg_sample), newp, newlen);
	if (ret != 0) {
		return ret;
	}
	if (lg_sample >= (sizeof(uint64_t) << 3)) {
		lg_sample = (sizeof(uint64_t) << 3) - 1;
	}

	prof_reset(tsd, lg_sample);
	return 0;
}

CTL_RO_NL_CGEN_PUBLIC(config_prof, prof_interval, prof_interval, uint64_t)
CTL_RO_NL_CGEN_PUBLIC(config_prof, lg_prof_sample, lg_prof_sample, size_t)

int
prof_log_start_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	const char *filename = NULL;

	if (!config_prof || !opt_prof) {
		return ENOENT;
	}

	int ret = ctl_writeonly(oldp, oldlenp);
	if (ret != 0) {
		return ret;
	}

	ret = ctl_write(&filename, sizeof(filename), newp, newlen);
	if (ret != 0) {
		return ret;
	}

	return prof_log_start(tsd_tsdn(tsd), filename) ? EFAULT : 0;
}

int
prof_log_stop_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	if (!config_prof || !opt_prof) {
		return ENOENT;
	}

	if (prof_log_stop(tsd_tsdn(tsd))) {
		return EFAULT;
	}

	return 0;
}

int
prof_stats_bins_i_live_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	unsigned     binind = 0;
	prof_stats_t stats;

	if (!(config_prof && opt_prof && opt_prof_stats)) {
		return ENOENT;
	}

	int ret = ctl_readonly(newp, newlen);
	if (ret == 0) {
		ret = ctl_mib_unsigned(&binind, mib, 3);
	}
	if (ret == 0 && binind >= SC_NBINS) {
		ret = EINVAL;
	}
	if (ret == 0) {
		prof_stats_get_live(tsd, (szind_t)binind, &stats);
		ret = ctl_read(oldp, oldlenp, &stats, sizeof(stats));
	}
	return ret;
}

int
prof_stats_bins_i_accum_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	unsigned     binind = 0;
	prof_stats_t stats;

	if (!(config_prof && opt_prof && opt_prof_stats)) {
		return ENOENT;
	}

	int ret = ctl_readonly(newp, newlen);
	if (ret == 0) {
		ret = ctl_mib_unsigned(&binind, mib, 3);
	}
	if (ret == 0 && binind >= SC_NBINS) {
		ret = EINVAL;
	}
	if (ret == 0) {
		prof_stats_get_accum(tsd, (szind_t)binind, &stats);
		ret = ctl_read(oldp, oldlenp, &stats, sizeof(stats));
	}
	return ret;
}

int
prof_stats_lextents_i_live_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	unsigned     lextent_ind = 0;
	prof_stats_t stats;

	if (!(config_prof && opt_prof && opt_prof_stats)) {
		return ENOENT;
	}

	int ret = ctl_readonly(newp, newlen);
	if (ret == 0) {
		ret = ctl_mib_unsigned(&lextent_ind, mib, 3);
	}
	if (ret == 0 && lextent_ind >= SC_NSIZES - SC_NBINS) {
		ret = EINVAL;
	}
	if (ret == 0) {
		prof_stats_get_live(
		    tsd, (szind_t)(lextent_ind + SC_NBINS), &stats);
		ret = ctl_read(oldp, oldlenp, &stats, sizeof(stats));
	}
	return ret;
}

int
prof_stats_lextents_i_accum_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	unsigned     lextent_ind = 0;
	prof_stats_t stats;

	if (!(config_prof && opt_prof && opt_prof_stats)) {
		return ENOENT;
	}

	int ret = ctl_readonly(newp, newlen);
	if (ret == 0) {
		ret = ctl_mib_unsigned(&lextent_ind, mib, 3);
	}
	if (ret == 0 && lextent_ind >= SC_NSIZES - SC_NBINS) {
		ret = EINVAL;
	}
	if (ret == 0) {
		prof_stats_get_accum(
		    tsd, (szind_t)(lextent_ind + SC_NBINS), &stats);
		ret = ctl_read(oldp, oldlenp, &stats, sizeof(stats));
	}
	return ret;
}

/******************************************************************************/
/* experimental.prof_recent.* and experimental.hooks.prof_* mallctl handlers. */

#define PROF_HOOK_CTL_BODY(hook_type, hook_get, hook_set, allow_null)          \
	do {                                                                          \
		int ret;                                                                     \
		if (oldp == NULL && newp == NULL) {                                          \
			ret = EINVAL;                                                               \
			goto label_return;                                                          \
		}                                                                            \
		if (oldp != NULL) {                                                          \
			hook_type old_hook = hook_get();                                            \
			ret = ctl_read(oldp, oldlenp, &old_hook,                                    \
			    sizeof(hook_type));                                                     \
			if (ret != 0) {                                                             \
				goto label_return;                                                         \
			}                                                                           \
		}                                                                            \
		if (newp != NULL) {                                                          \
			if (!opt_prof) {                                                            \
				ret = ENOENT;                                                              \
				goto label_return;                                                         \
			}                                                                           \
			hook_type new_hook JEMALLOC_CC_SILENCE_INIT(NULL);                          \
			ret = ctl_write(&new_hook, sizeof(hook_type), newp,                         \
			    newlen);                                                                \
			if (ret != 0) {                                                             \
				goto label_return;                                                         \
			}                                                                           \
			if (!(allow_null) && new_hook == NULL) {                                    \
				ret = EINVAL;                                                              \
				goto label_return;                                                         \
			}                                                                           \
			hook_set(new_hook);                                                         \
		}                                                                            \
		ret = 0;                                                                     \
	label_return:                                                                 \
		return ret;                                                                  \
	} while (0)

int
experimental_hooks_prof_backtrace_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	PROF_HOOK_CTL_BODY(prof_backtrace_hook_t, prof_backtrace_hook_get,
	    prof_backtrace_hook_set, false);
}

int
experimental_hooks_prof_dump_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	PROF_HOOK_CTL_BODY(prof_dump_hook_t, prof_dump_hook_get,
	    prof_dump_hook_set, true);
}

int
experimental_hooks_prof_sample_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	PROF_HOOK_CTL_BODY(prof_sample_hook_t, prof_sample_hook_get,
	    prof_sample_hook_set, true);
}

int
experimental_hooks_prof_sample_free_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	PROF_HOOK_CTL_BODY(prof_sample_free_hook_t, prof_sample_free_hook_get,
	    prof_sample_free_hook_set, true);
}

#undef PROF_HOOK_CTL_BODY

int
experimental_prof_recent_alloc_max_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	if (!(config_prof && opt_prof)) {
		return ENOENT;
	}

	ssize_t old_max = 0;
	int     ret = 0;
	if (newp != NULL) {
		ssize_t max;
		ret = ctl_write(&max, sizeof(max), newp, newlen);
		if (ret == 0 && max < -1) {
			ret = EINVAL;
		}
		if (ret == 0) {
			old_max = prof_recent_alloc_max_ctl_write(tsd, max);
		}
	} else {
		old_max = prof_recent_alloc_max_ctl_read();
	}
	if (ret == 0) {
		ret = ctl_read(oldp, oldlenp, &old_max, sizeof(old_max));
	}
	return ret;
}

typedef struct write_cb_packet_s write_cb_packet_t;
struct write_cb_packet_s {
	write_cb_t *write_cb;
	void       *cbopaque;
};

int
experimental_prof_recent_alloc_dump_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	if (!(config_prof && opt_prof)) {
		return ENOENT;
	}

	assert(sizeof(write_cb_packet_t) == sizeof(void *) * 2);

	int ret = ctl_writeonly(oldp, oldlenp);
	if (ret != 0) {
		return ret;
	}

	write_cb_packet_t write_cb_packet;
	ret = ctl_assured_write(
	    &write_cb_packet, sizeof(write_cb_packet), newp, newlen);
	if (ret != 0) {
		return ret;
	}

	prof_recent_alloc_dump(
	    tsd, write_cb_packet.write_cb, write_cb_packet.cbopaque);
	return 0;
}
