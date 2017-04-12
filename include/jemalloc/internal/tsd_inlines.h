#ifndef JEMALLOC_INTERNAL_TSD_INLINES_H
#define JEMALLOC_INTERNAL_TSD_INLINES_H

#ifndef JEMALLOC_ENABLE_INLINE
malloc_tsd_protos(JEMALLOC_ATTR(unused), , tsd_t)

tsd_t *tsd_fetch_impl(bool init);
tsd_t *tsd_fetch(void);
tsdn_t *tsd_tsdn(tsd_t *tsd);
bool tsd_nominal(tsd_t *tsd);
#define O(n, t, gs, i, c)						\
t *tsd_##n##p_get(tsd_t *tsd);						\
t tsd_##n##_get(tsd_t *tsd);						\
void tsd_##n##_set(tsd_t *tsd, t n);
MALLOC_TSD
#undef O
tsdn_t *tsdn_fetch(void);
bool tsdn_null(const tsdn_t *tsdn);
tsd_t *tsdn_tsd(tsdn_t *tsdn);
rtree_ctx_t *tsd_rtree_ctx(tsd_t *tsd);
rtree_ctx_t *tsdn_rtree_ctx(tsdn_t *tsdn, rtree_ctx_t *fallback);
bool tsd_fast(tsd_t *tsd);
void tsd_assert_fast(tsd_t *tsd);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_TSD_C_))
malloc_tsd_externs(, tsd_t)
malloc_tsd_funcs(JEMALLOC_ALWAYS_INLINE, , tsd_t, tsd_initializer, tsd_cleanup)

#define MALLOC_TSD_getset_yes(n, t)					\
JEMALLOC_ALWAYS_INLINE t						\
tsd_##n##_get(tsd_t *tsd) {						\
	return *tsd_##n##p_get(tsd);					\
}									\
JEMALLOC_ALWAYS_INLINE void						\
tsd_##n##_set(tsd_t *tsd, t n) {					\
	assert(tsd->state == tsd_state_nominal ||			\
	    tsd->state == tsd_state_nominal_slow ||			\
	    tsd->state == tsd_state_reincarnated);			\
	tsd->n = n;							\
}
#define MALLOC_TSD_getset_no(n, t)
#define O(n, t, gs, i, c)						\
JEMALLOC_ALWAYS_INLINE t *						\
tsd_##n##p_get(tsd_t *tsd) {						\
	return &tsd->n;							\
}									\
									\
MALLOC_TSD_getset_##gs(n, t)
MALLOC_TSD
#undef MALLOC_TSD_getset_yes
#undef MALLOC_TSD_getset_no
#undef O

JEMALLOC_ALWAYS_INLINE void
tsd_assert_fast(tsd_t *tsd) {
	assert(!malloc_slow && tsd_tcache_enabled_get(tsd));
}

JEMALLOC_ALWAYS_INLINE bool
tsd_fast(tsd_t *tsd) {
	bool fast = (tsd->state == tsd_state_nominal);
	if (fast) {
		tsd_assert_fast(tsd);
	}

	return fast;
}

JEMALLOC_ALWAYS_INLINE tsd_t *
tsd_fetch_impl(bool init) {
	tsd_t *tsd = tsd_get(init);

	if (!init && tsd_get_allocates() && tsd == NULL) {
		return NULL;
	}
	assert(tsd != NULL);

	if (unlikely(tsd->state != tsd_state_nominal)) {
		return tsd_fetch_slow(tsd);
	}
	assert(tsd_fast(tsd));
	tsd_assert_fast(tsd);

	return tsd;
}

JEMALLOC_ALWAYS_INLINE tsd_t *
tsd_fetch(void) {
	return tsd_fetch_impl(true);
}

JEMALLOC_ALWAYS_INLINE tsdn_t *
tsd_tsdn(tsd_t *tsd) {
	return (tsdn_t *)tsd;
}

JEMALLOC_INLINE bool
tsd_nominal(tsd_t *tsd) {
	return (tsd->state <= tsd_state_nominal_max);
}

JEMALLOC_ALWAYS_INLINE tsdn_t *
tsdn_fetch(void) {
	if (!tsd_booted_get()) {
		return NULL;
	}

	return tsd_tsdn(tsd_fetch_impl(false));
}

JEMALLOC_ALWAYS_INLINE bool
tsdn_null(const tsdn_t *tsdn) {
	return tsdn == NULL;
}

JEMALLOC_ALWAYS_INLINE tsd_t *
tsdn_tsd(tsdn_t *tsdn) {
	assert(!tsdn_null(tsdn));

	return &tsdn->tsd;
}

JEMALLOC_ALWAYS_INLINE rtree_ctx_t *
tsd_rtree_ctx(tsd_t *tsd) {
	return tsd_rtree_ctxp_get(tsd);
}

JEMALLOC_ALWAYS_INLINE rtree_ctx_t *
tsdn_rtree_ctx(tsdn_t *tsdn, rtree_ctx_t *fallback) {
	/*
	 * If tsd cannot be accessed, initialize the fallback rtree_ctx and
	 * return a pointer to it.
	 */
	if (unlikely(tsdn_null(tsdn))) {
		rtree_ctx_data_init(fallback);
		return fallback;
	}
	return tsd_rtree_ctx(tsdn_tsd(tsdn));
}
#endif

#endif /* JEMALLOC_INTERNAL_TSD_INLINES_H */
