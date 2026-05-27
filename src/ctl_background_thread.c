#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/background_thread.h"
#include "jemalloc/internal/background_thread_inlines.h"
#include "jemalloc/internal/ctl_mallctl.h"

/******************************************************************************/
/* background_thread mallctl handlers. */

int
background_thread_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	int  ret;
	bool oldval = false;

	if (!have_background_thread) {
		return ENOENT;
	}
	background_thread_ctl_init(tsd_tsdn(tsd));

	ctl_mtx_lock(tsd_tsdn(tsd));
	malloc_mutex_lock(tsd_tsdn(tsd), &background_thread_lock);

	if (newp != NULL && newlen != sizeof(bool)) {
		ret = EINVAL;
		goto label_return;
	}
	oldval = background_thread_enabled();
	ret = ctl_read(oldp, oldlenp, &oldval, sizeof(oldval));
	if (ret != 0 || newp == NULL) {
		goto label_return;
	}

	bool newval;
	memcpy(&newval, newp, sizeof(newval));
	if (newval != oldval) {
		background_thread_enabled_set(tsd_tsdn(tsd), newval);
		if (newval) {
			if (background_threads_enable(tsd)) {
				ret = EFAULT;
			}
		} else {
			if (background_threads_disable(tsd)) {
				ret = EFAULT;
			}
		}
	}
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &background_thread_lock);
	ctl_mtx_unlock(tsd_tsdn(tsd));

	return ret;
}

int
max_background_threads_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,
    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {
	int    ret;
	size_t oldval = 0;

	if (!have_background_thread) {
		return ENOENT;
	}
	background_thread_ctl_init(tsd_tsdn(tsd));

	ctl_mtx_lock(tsd_tsdn(tsd));
	malloc_mutex_lock(tsd_tsdn(tsd), &background_thread_lock);

	if (newp != NULL && newlen != sizeof(size_t)) {
		ret = EINVAL;
		goto label_return;
	}
	oldval = max_background_threads;
	ret = ctl_read(oldp, oldlenp, &oldval, sizeof(oldval));
	if (ret != 0 || newp == NULL) {
		goto label_return;
	}

	size_t newval;
	memcpy(&newval, newp, sizeof(newval));
	if (newval == oldval) {
		goto label_return;
	}
	if (newval > opt_max_background_threads || newval == 0) {
		ret = EINVAL;
		goto label_return;
	}
	if (background_thread_enabled()) {
		background_thread_enabled_set(tsd_tsdn(tsd), false);
		if (background_threads_disable(tsd)) {
			ret = EFAULT;
			goto label_return;
		}
		max_background_threads = newval;
		background_thread_enabled_set(tsd_tsdn(tsd), true);
		if (background_threads_enable(tsd)) {
			ret = EFAULT;
			goto label_return;
		}
	} else {
		max_background_threads = newval;
	}
label_return:
	malloc_mutex_unlock(tsd_tsdn(tsd), &background_thread_lock);
	ctl_mtx_unlock(tsd_tsdn(tsd));

	return ret;
}
