#ifdef JEMALLOC_INTERNAL_TSD_WIN_H
#error This file should be included only once, by tsd.h.
#endif
#define JEMALLOC_INTERNAL_TSD_WIN_H

extern __declspec(thread) tsd_t tsd_tls;
extern bool tsd_booted;

/* Initialization/cleanup. */
JEMALLOC_ALWAYS_INLINE bool
tsd_boot0(void) {
	tsd_booted = true;
	return false;
}

JEMALLOC_ALWAYS_INLINE void
tsd_boot1(void) {
	/* Do nothing. */
}

JEMALLOC_ALWAYS_INLINE bool
tsd_boot(void) {
	return tsd_boot0();
}

JEMALLOC_ALWAYS_INLINE bool
tsd_booted_get(void) {
	return tsd_booted;
}

JEMALLOC_ALWAYS_INLINE bool
tsd_get_allocates(void) {
	return false;
}

/* Get/set. */
JEMALLOC_ALWAYS_INLINE tsd_t*
tsd_get(UNUSED bool init) {
	return &tsd_tls;
}

JEMALLOC_ALWAYS_INLINE void
tsd_set(tsd_t* val) {
	if (likely(&tsd_tls != val)) {
		tsd_tls = (*val);
	}
}
