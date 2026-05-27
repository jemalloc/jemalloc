#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/ctl_mallctl.h"

/******************************************************************************/
/* config.* mallctl handlers. */

#define CTL_RO_CONFIG_GEN(n, t)                                                \
	int n##_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,                     \
	    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {                 \
		int ret = ctl_readonly(newp, newlen);                                        \
		if (ret == 0) {                                                              \
			t oldval = n;                                                               \
			ret = ctl_read(oldp, oldlenp, &oldval, sizeof(t));                          \
		}                                                                            \
		return ret;                                                                  \
	}

CTL_RO_CONFIG_GEN(config_cache_oblivious, bool)
CTL_RO_CONFIG_GEN(config_debug, bool)
CTL_RO_CONFIG_GEN(config_fill, bool)
CTL_RO_CONFIG_GEN(config_infallible_new, bool)
CTL_RO_CONFIG_GEN(config_lazy_lock, bool)
CTL_RO_CONFIG_GEN(config_malloc_conf, const char *)
CTL_RO_CONFIG_GEN(config_opt_safety_checks, bool)
CTL_RO_CONFIG_GEN(config_prof, bool)
CTL_RO_CONFIG_GEN(config_prof_libgcc, bool)
CTL_RO_CONFIG_GEN(config_prof_libunwind, bool)
CTL_RO_CONFIG_GEN(config_prof_frameptr, bool)
CTL_RO_CONFIG_GEN(config_stats, bool)
CTL_RO_CONFIG_GEN(config_utrace, bool)
CTL_RO_CONFIG_GEN(config_xmalloc, bool)

#undef CTL_RO_CONFIG_GEN
