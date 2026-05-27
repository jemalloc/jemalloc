#ifndef JEMALLOC_INTERNAL_CTL_MALLCTL_H
#define JEMALLOC_INTERNAL_CTL_MALLCTL_H

#include "jemalloc/internal/ctl.h"

/*
 * Shared helpers for the mallctl handlers that the ctl_* namespace modules
 * implement.  These replace the former READ/WRITE/... macros with memcpy-based
 * functions.  Helpers used by more than one source file live here as
 * static inline; helpers used by a single module are defined JET_EXTERN in that
 * module instead (e.g. ctl_read_xor_write in ctl_thread.c).
 */

static inline int
ctl_writeonly(void *oldp, size_t *oldlenp) {
	if (oldp != NULL || oldlenp != NULL) {
		return EPERM;
	}
	return 0;
}

static inline int
ctl_assured_write(void *dst, size_t dst_size, const void *newp,
    size_t newlen) {
	if (newp == NULL || newlen != dst_size) {
		return EINVAL;
	}
	memcpy(dst, newp, dst_size);
	return 0;
}

static inline int
ctl_read(void *oldp, size_t *oldlenp, const void *src, size_t src_size) {
	if (oldp == NULL || oldlenp == NULL) {
		return 0;
	}
	const size_t oldlen = *oldlenp;
	if (oldlen != src_size) {
		size_t copylen = (src_size <= oldlen) ? src_size : oldlen;
		memcpy(oldp, src, copylen);
		*oldlenp = copylen;
		return EINVAL;
	}
	memcpy(oldp, src, src_size);
	return 0;
}

static inline int
ctl_write(void *dst, size_t dst_size, const void *newp, size_t newlen) {
	if (newp == NULL) {
		return 0;
	}
	if (newlen != dst_size) {
		return EINVAL;
	}
	memcpy(dst, newp, dst_size);
	return 0;
}

static inline int
ctl_readonly(const void *newp, size_t newlen) {
	if (newp != NULL || newlen != 0) {
		return EPERM;
	}
	return 0;
}

static inline int
ctl_neither_read_nor_write(void *oldp, size_t *oldlenp, const void *newp,
    size_t newlen) {
	if (oldp != NULL || oldlenp != NULL || newp != NULL || newlen != 0) {
		return EPERM;
	}
	return 0;
}

static inline int
ctl_verify_read(void *oldp, size_t *oldlenp, size_t expected_size) {
	if (oldp == NULL || oldlenp == NULL || *oldlenp != expected_size) {
		if (oldlenp != NULL) {
			*oldlenp = 0;
		}
		return EINVAL;
	}
	return 0;
}

static inline int
ctl_mib_unsigned(unsigned *dst, const size_t *mib, size_t mib_index) {
	const size_t value = mib[mib_index];
	if (value > UINT_MAX) {
		return EFAULT;
	}
	*dst = (unsigned)value;
	return 0;
}

/*
 * Read-only handler generators for the split modules.  These mirror the
 * CTL_RO_* generators in ctl.c but emit externally linked handlers (referenced
 * from the mallctl tree in ctl.c) and reach ctl_mtx through its accessors,
 * since ctl_mtx itself is private to ctl.c.
 */
#define CTL_RO_GEN_PUBLIC(n, v, t)                                             \
	int n##_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,             \
	    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {          \
		ctl_mtx_lock(tsd_tsdn(tsd));                                   \
		int ret = ctl_readonly(newp, newlen);                          \
		if (ret == 0) {                                                \
			t oldval = (v);                                        \
			ret = ctl_read(oldp, oldlenp, &oldval, sizeof(t));     \
		}                                                              \
		ctl_mtx_unlock(tsd_tsdn(tsd));                                 \
		return ret;                                                    \
	}

#define CTL_RO_CGEN_PUBLIC(c, n, v, t)                                        \
	int n##_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,             \
	    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {          \
		if (!(c)) {                                                    \
			return ENOENT;                                         \
		}                                                              \
		ctl_mtx_lock(tsd_tsdn(tsd));                                   \
		int ret = ctl_readonly(newp, newlen);                          \
		if (ret == 0) {                                                \
			t oldval = (v);                                        \
			ret = ctl_read(oldp, oldlenp, &oldval, sizeof(t));     \
		}                                                              \
		ctl_mtx_unlock(tsd_tsdn(tsd));                                 \
		return ret;                                                    \
	}

/*
 * ctl_mtx is not acquired, under the assumption that no pertinent data will
 * mutate during the call.
 */
#define CTL_RO_NL_GEN_PUBLIC(n, v, t)                                         \
	int n##_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,             \
	    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {          \
		int ret = ctl_readonly(newp, newlen);                          \
		if (ret == 0) {                                                \
			t oldval = (v);                                        \
			ret = ctl_read(oldp, oldlenp, &oldval, sizeof(t));     \
		}                                                              \
		return ret;                                                    \
	}

#define CTL_RO_NL_CGEN_PUBLIC(c, n, v, t)                                     \
	int n##_ctl(tsd_t *tsd, const size_t *mib, size_t miblen,             \
	    void *oldp, size_t *oldlenp, void *newp, size_t newlen) {          \
		if (!(c)) {                                                    \
			return ENOENT;                                         \
		}                                                              \
		int ret = ctl_readonly(newp, newlen);                          \
		if (ret == 0) {                                                \
			t oldval = (v);                                        \
			ret = ctl_read(oldp, oldlenp, &oldval, sizeof(t));     \
		}                                                              \
		return ret;                                                    \
	}

#endif /* JEMALLOC_INTERNAL_CTL_MALLCTL_H */
