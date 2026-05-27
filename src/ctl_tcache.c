#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/ctl_mallctl.h"

/******************************************************************************/
/* tcache.* mallctl handlers. */

int
tcache_create_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	unsigned tcache_ind;

	int ret = ctl_readonly(newp, newlen);
	if (ret == 0) {
		ret = ctl_verify_read(oldp, oldlenp, sizeof(tcache_ind));
	}
	if (ret == 0) {
		if (tcaches_create(tsd, b0get(), &tcache_ind)) {
			ret = EFAULT;
		} else {
			ret = ctl_read(oldp, oldlenp, &tcache_ind,
			    sizeof(tcache_ind));
		}
	}
	return ret;
}

int
tcache_flush_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	unsigned tcache_ind;

	int ret = ctl_writeonly(oldp, oldlenp);
	if (ret != 0) {
		return ret;
	}

	ret = ctl_assured_write(&tcache_ind, sizeof(tcache_ind), newp, newlen);
	if (ret != 0) {
		return ret;
	}

	tcaches_flush(tsd, tcache_ind);
	return 0;
}

int
tcache_destroy_ctl(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen) {
	unsigned tcache_ind;

	int ret = ctl_writeonly(oldp, oldlenp);
	if (ret != 0) {
		return ret;
	}

	ret = ctl_assured_write(&tcache_ind, sizeof(tcache_ind), newp, newlen);
	if (ret != 0) {
		return ret;
	}

	tcaches_destroy(tsd, tcache_ind);
	return 0;
}
