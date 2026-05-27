#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/arena.h"
#include "jemalloc/internal/arena_inlines.h"
#include "jemalloc/internal/arenas_management.h"
#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/ctl_mallctl.h"
#include "jemalloc/internal/peak_event.h"
#include "jemalloc/internal/prof.h"
#include "jemalloc/internal/prof_data.h"
#include "jemalloc/internal/sc.h"
#include "jemalloc/internal/tcache_inlines.h"
#include "jemalloc/internal/thread_event_registry.h"

/*
 * ctl_read_xor_write is used only by this module, so (unlike the shared helpers
 * in ctl_mallctl.h) it lives here: static in normal builds, externally linked
 * under JET so the unit tests can exercise it directly.
 */
#ifdef JEMALLOC_JET
int ctl_read_xor_write(void *oldp, size_t *oldlenp, const void *newp,
    size_t newlen);
#endif
JET_EXTERN int
ctl_read_xor_write(void *oldp, size_t *oldlenp, const void *newp,
    size_t newlen) {
	if ((oldp != NULL && oldlenp != NULL)
	    && (newp != NULL || newlen != 0)) {
		return EPERM;
	}
	return 0;
}

/*******************************************************************************/
/* thread.* mallctl handlers. */

int
thread_arena_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	arena_t *oldarena;
	unsigned newind, oldind;

	oldarena = arena_choose(tsd, NULL);
	if (oldarena == NULL) {
		return EAGAIN;
	}
	newind = oldind = arena_ind_get(oldarena);
	int ret = ctl_write(&newind, sizeof(newind), newp, newlen);
	if (ret != 0) {
		return ret;
	}
	ret = ctl_read(oldp, oldlenp, &oldind, sizeof(oldind));
	if (ret != 0) {
		return ret;
	}

	if (newind == oldind) {
		return 0;
	}

	if (newind >= narenas_total_get()) {
		/* New arena index is out of range. */
		return EFAULT;
	}

	if (have_percpu_arena && PERCPU_ARENA_ENABLED(opt_percpu_arena)) {
		if (newind < percpu_arena_ind_limit(opt_percpu_arena)) {
			/*
			 * If perCPU arena is enabled, thread_arena control is
			 * not allowed for the auto arena range.
			 */
			return EPERM;
		}
	}

	/* Initialize arena if necessary. */
	arena_t *newarena = arena_get(tsd_tsdn(tsd), newind, true);
	if (newarena == NULL) {
		return EAGAIN;
	}
	thread_migrate_arena(tsd, oldarena, newarena);

	return 0;
}
CTL_RO_NL_GEN_PUBLIC(thread_allocated, tsd_thread_allocated_get(tsd), uint64_t)
CTL_RO_NL_GEN_PUBLIC(thread_allocatedp, tsd_thread_allocatedp_get(tsd), uint64_t *)

int
thread_tcache_ncached_max_read_sizeclass_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	size_t bin_size = 0;

	/* Read the bin size from newp. */
	int ret = ctl_assured_write(&bin_size, sizeof(bin_size), newp, newlen);
	if (ret != 0) {
		return ret;
	}

	cache_bin_sz_t ncached_max = 0;
	if (tcache_bin_ncached_max_read(tsd, bin_size, &ncached_max)) {
		return EINVAL;
	}
	size_t result = (size_t)ncached_max;
	return ctl_read(oldp, oldlenp, &result, sizeof(result));
}

int
thread_tcache_ncached_max_write_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int ret = ctl_writeonly(oldp, oldlenp);
	if (ret != 0) {
		return ret;
	}
	if (newp == NULL) {
		return 0;
	}
	if (!tcache_available(tsd)) {
		return ENOENT;
	}

	char *settings = NULL;
	ret = ctl_write(&settings, sizeof(settings), newp, newlen);
	if (ret != 0) {
		return ret;
	}
	if (settings == NULL) {
		return EINVAL;
	}
	/* Get the length of the setting string safely. */
	char *end = (char *)memchr(
	    settings, '\0', CTL_MULTI_SETTING_MAX_LEN);
	if (end == NULL) {
		return EINVAL;
	}
	/*
	 * Exclude the last '\0' for len since it is not handled by
	 * multi_setting_parse_next.
	 */
	size_t len = (uintptr_t)end - (uintptr_t)settings;
	if (len == 0) {
		return 0;
	}

	return tcache_bins_ncached_max_write(tsd, settings, len) ? EINVAL : 0;
}

CTL_RO_NL_GEN_PUBLIC(thread_deallocated, tsd_thread_deallocated_get(tsd), uint64_t)
CTL_RO_NL_GEN_PUBLIC(thread_deallocatedp, tsd_thread_deallocatedp_get(tsd), uint64_t *)

int
thread_tcache_enabled_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	bool oldval = tcache_enabled_get(tsd);

	bool newval = false;
	int ret = ctl_write(&newval, sizeof(newval), newp, newlen);
	if (ret == 0 && newp != NULL) {
		tcache_enabled_set(tsd, newval);
	}
	if (ret == 0) {
		ret = ctl_read(oldp, oldlenp, &oldval, sizeof(oldval));
	}
	return ret;
}

int
thread_tcache_max_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	size_t oldval;

	/* pointer to tcache_t always exists even with tcache disabled. */
	tcache_t *tcache = tsd_tcachep_get(tsd);
	assert(tcache != NULL);
	oldval = tcache_max_get(tcache->tcache_slow);
	int ret = ctl_read(oldp, oldlenp, &oldval, sizeof(oldval));
	if (ret != 0) {
		return ret;
	}

	size_t new_tcache_max = oldval;
	ret = ctl_write(&new_tcache_max, sizeof(new_tcache_max), newp, newlen);
	if (ret != 0) {
		return ret;
	}
	if (newp != NULL) {
		if (new_tcache_max > TCACHE_MAXCLASS_LIMIT) {
			new_tcache_max = TCACHE_MAXCLASS_LIMIT;
		}
		new_tcache_max = sz_s2u(new_tcache_max);
		if (new_tcache_max != oldval) {
			thread_tcache_max_set(tsd, new_tcache_max);
		}
	}

	return 0;
}

int
thread_tcache_flush_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	if (!tcache_available(tsd)) {
		return EFAULT;
	}

	int ret = ctl_neither_read_nor_write(oldp, oldlenp, newp, newlen);
	if (ret == 0) {
		tcache_flush(tsd);
	}
	return ret;
}

int
thread_peak_read_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	if (!config_stats) {
		return ENOENT;
	}
	int ret = ctl_readonly(newp, newlen);
	if (ret == 0) {
		peak_event_update(tsd);
		uint64_t result = peak_event_max(tsd);
		ret = ctl_read(oldp, oldlenp, &result, sizeof(result));
	}
	return ret;
}

int
thread_peak_reset_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	if (!config_stats) {
		return ENOENT;
	}
	int ret = ctl_neither_read_nor_write(oldp, oldlenp, newp, newlen);
	if (ret == 0) {
		peak_event_zero(tsd);
	}
	return ret;
}

int
thread_prof_name_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	if (!config_prof || !opt_prof) {
		return ENOENT;
	}

	int ret = ctl_read_xor_write(oldp, oldlenp, newp, newlen);
	if (ret == 0 && newp != NULL) {
		const char *newval = NULL;
		ret = ctl_write(&newval, sizeof(newval), newp, newlen);
		if (ret == 0) {
			if (newval == NULL) {
				ret = EINVAL;
			} else {
				ret = prof_thread_name_set(tsd, newval);
			}
		}
	} else if (ret == 0) {
		const char *oldname = prof_thread_name_get(tsd);
		ret = ctl_read(oldp, oldlenp, &oldname, sizeof(oldname));
	}
	return ret;
}

int
thread_prof_active_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	if (!config_prof) {
		return ENOENT;
	}

	bool oldval = opt_prof ? prof_thread_active_get(tsd) : false;
	int  ret = 0;
	if (newp != NULL) {
		if (!opt_prof) {
			ret = ENOENT;
		} else {
			bool newval;
			ret = ctl_write(&newval, sizeof(newval), newp, newlen);
			if (ret == 0 && prof_thread_active_set(tsd, newval)) {
				ret = EAGAIN;
			}
		}
	}
	if (ret == 0) {
		ret = ctl_read(oldp, oldlenp, &oldval, sizeof(oldval));
	}
	return ret;
}

int
thread_idle_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	int ret = ctl_neither_read_nor_write(oldp, oldlenp, newp, newlen);
	if (ret != 0) {
		return ret;
	}

	if (tcache_available(tsd)) {
		tcache_flush(tsd);
	}
	/*
	 * This heuristic is perhaps not the most well-considered.  But it
	 * matches the only idling policy we have experience with in the status
	 * quo.  Over time we should investigate more principled approaches.
	 */
	if (opt_narenas > ncpus * 2) {
		arena_t *arena = arena_choose(tsd, NULL);
		if (arena != NULL) {
			arena_decay(tsd_tsdn(tsd), arena, false, true);
		}
		/*
		 * The missing arena case is not actually an error; a thread
		 * might be idle before it associates itself to one.  This is
		 * unusual, but not wrong.
		 */
	}

	return 0;
}

int
experimental_hooks_thread_event_ctl(tsd_t *tsd, const size_t *mib,
    size_t miblen, void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	user_hook_object_t t_new = {NULL, 0, false};

	int ret = ctl_assured_write(&t_new, sizeof(t_new), newp, newlen);
	if (ret != 0) {
		return ret;
	}

	return te_register_user_handler(tsd_tsdn(tsd), &t_new);
}
