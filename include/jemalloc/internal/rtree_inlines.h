#ifndef JEMALLOC_INTERNAL_RTREE_INLINES_H
#define JEMALLOC_INTERNAL_RTREE_INLINES_H

#ifndef JEMALLOC_ENABLE_INLINE
uintptr_t rtree_leafkey(uintptr_t key);
uintptr_t rtree_subkey(uintptr_t key, unsigned level);
#  ifdef RTREE_LEAF_COMPACT
uintptr_t rtree_leaf_elm_bits_read(tsdn_t *tsdn, rtree_t *rtree,
    rtree_leaf_elm_t *elm, bool acquired, bool dependent);
extent_t *rtree_leaf_elm_bits_extent_get(uintptr_t bits);
szind_t rtree_leaf_elm_bits_szind_get(uintptr_t bits);
bool rtree_leaf_elm_bits_slab_get(uintptr_t bits);
bool rtree_leaf_elm_bits_locked_get(uintptr_t bits);
#  endif
extent_t *rtree_leaf_elm_extent_read(tsdn_t *tsdn, rtree_t *rtree,
    rtree_leaf_elm_t *elm, bool acquired, bool dependent);
szind_t rtree_leaf_elm_szind_read(tsdn_t *tsdn, rtree_t *rtree,
    rtree_leaf_elm_t *elm, bool acquired, bool dependent);
bool rtree_leaf_elm_slab_read(tsdn_t *tsdn, rtree_t *rtree,
    rtree_leaf_elm_t *elm, bool acquired, bool dependent);
void rtree_leaf_elm_extent_write(tsdn_t *tsdn, rtree_t *rtree,
    rtree_leaf_elm_t *elm, bool acquired, extent_t *extent);
void rtree_leaf_elm_szind_write(tsdn_t *tsdn, rtree_t *rtree,
    rtree_leaf_elm_t *elm, bool acquired, szind_t szind);
void rtree_leaf_elm_slab_write(tsdn_t *tsdn, rtree_t *rtree,
    rtree_leaf_elm_t *elm, bool acquired, bool slab);
void rtree_leaf_elm_write(tsdn_t *tsdn, rtree_t *rtree, rtree_leaf_elm_t *elm,
    bool acquired, extent_t *extent, szind_t szind, bool slab);
void rtree_leaf_elm_szind_slab_update(tsdn_t *tsdn, rtree_t *rtree,
    rtree_leaf_elm_t *elm, szind_t szind, bool slab);
rtree_leaf_elm_t *rtree_leaf_elm_lookup(tsdn_t *tsdn, rtree_t *rtree,
    rtree_ctx_t *rtree_ctx, uintptr_t key, bool dependent, bool init_missing);
bool rtree_write(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key, extent_t *extent, szind_t szind, bool slab);
rtree_leaf_elm_t *rtree_read(tsdn_t *tsdn, rtree_t *rtree,
    rtree_ctx_t *rtree_ctx, uintptr_t key, bool dependent);
extent_t *rtree_extent_read(tsdn_t *tsdn, rtree_t *rtree,
    rtree_ctx_t *rtree_ctx, uintptr_t key, bool dependent);
szind_t rtree_szind_read(tsdn_t *tsdn, rtree_t *rtree,
    rtree_ctx_t *rtree_ctx, uintptr_t key, bool dependent);
bool rtree_extent_szind_read(tsdn_t *tsdn, rtree_t *rtree,
    rtree_ctx_t *rtree_ctx, uintptr_t key, bool dependent, extent_t **r_extent,
    szind_t *r_szind);
bool rtree_szind_slab_read(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key, bool dependent, szind_t *r_szind, bool *r_slab);
rtree_leaf_elm_t *rtree_leaf_elm_acquire(tsdn_t *tsdn, rtree_t *rtree,
    rtree_ctx_t *rtree_ctx, uintptr_t key, bool dependent, bool init_missing);
void rtree_leaf_elm_release(tsdn_t *tsdn, rtree_t *rtree,
    rtree_leaf_elm_t *elm);
void rtree_szind_slab_update(tsdn_t *tsdn, rtree_t *rtree,
    rtree_ctx_t *rtree_ctx, uintptr_t key, szind_t szind, bool slab);
void rtree_clear(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_RTREE_C_))
JEMALLOC_ALWAYS_INLINE uintptr_t
rtree_leafkey(uintptr_t key) {
	unsigned ptrbits = ZU(1) << (LG_SIZEOF_PTR+3);
	unsigned cumbits = (rtree_levels[RTREE_HEIGHT-1].cumbits -
	    rtree_levels[RTREE_HEIGHT-1].bits);
	unsigned maskbits = ptrbits - cumbits;
	uintptr_t mask = ~((ZU(1) << maskbits) - 1);
	return (key & mask);
}

JEMALLOC_ALWAYS_INLINE uintptr_t
rtree_subkey(uintptr_t key, unsigned level) {
	unsigned ptrbits = ZU(1) << (LG_SIZEOF_PTR+3);
	unsigned cumbits = rtree_levels[level].cumbits;
	unsigned shiftbits = ptrbits - cumbits;
	unsigned maskbits = rtree_levels[level].bits;
	uintptr_t mask = (ZU(1) << maskbits) - 1;
	return ((key >> shiftbits) & mask);
}

/*
 * Atomic getters.
 *
 * dependent: Reading a value on behalf of a pointer to a valid allocation
 *            is guaranteed to be a clean read even without synchronization,
 *            because the rtree update became visible in memory before the
 *            pointer came into existence.
 * !dependent: An arbitrary read, e.g. on behalf of ivsalloc(), may not be
 *             dependent on a previous rtree write, which means a stale read
 *             could result if synchronization were omitted here.
 */
#  ifdef RTREE_LEAF_COMPACT
JEMALLOC_ALWAYS_INLINE uintptr_t
rtree_leaf_elm_bits_read(tsdn_t *tsdn, rtree_t *rtree, rtree_leaf_elm_t *elm,
    bool acquired, bool dependent) {
	if (config_debug && acquired) {
		assert(dependent);
		rtree_leaf_elm_witness_access(tsdn, rtree, elm);
	}

	return (uintptr_t)atomic_load_p(&elm->le_bits, dependent
	    ?  ATOMIC_RELAXED : ATOMIC_ACQUIRE);
}

JEMALLOC_ALWAYS_INLINE extent_t *
rtree_leaf_elm_bits_extent_get(uintptr_t bits) {
	/* Restore sign-extended high bits, mask slab and lock bits. */
	return (extent_t *)((uintptr_t)((intptr_t)(bits << RTREE_NHIB) >>
	    RTREE_NHIB) & ~((uintptr_t)0x3));
}

JEMALLOC_ALWAYS_INLINE szind_t
rtree_leaf_elm_bits_szind_get(uintptr_t bits) {
	return (szind_t)(bits >> LG_VADDR);
}

JEMALLOC_ALWAYS_INLINE bool
rtree_leaf_elm_bits_slab_get(uintptr_t bits) {
	return (bool)((bits >> 1) & (uintptr_t)0x1);
}

JEMALLOC_ALWAYS_INLINE bool
rtree_leaf_elm_bits_locked_get(uintptr_t bits) {
	return (bool)(bits & (uintptr_t)0x1);
}
#  endif

JEMALLOC_ALWAYS_INLINE extent_t *
rtree_leaf_elm_extent_read(tsdn_t *tsdn, rtree_t *rtree, rtree_leaf_elm_t *elm,
    bool acquired, bool dependent) {
	if (config_debug && acquired) {
		assert(dependent);
		rtree_leaf_elm_witness_access(tsdn, rtree, elm);
	}

#ifdef RTREE_LEAF_COMPACT
	uintptr_t bits = rtree_leaf_elm_bits_read(tsdn, rtree, elm, acquired,
	    dependent);
	assert(!acquired || rtree_leaf_elm_bits_locked_get(bits));
	return rtree_leaf_elm_bits_extent_get(bits);
#else
	extent_t *extent = (extent_t *)atomic_load_p(&elm->le_extent, dependent
	    ? ATOMIC_RELAXED : ATOMIC_ACQUIRE);
	assert(!acquired || ((uintptr_t)extent & (uintptr_t)0x1) ==
	    (uintptr_t)0x1);
	/* Mask lock bit. */
	extent = (extent_t *)((uintptr_t)extent & ~((uintptr_t)0x1));
	return extent;
#endif
}

JEMALLOC_ALWAYS_INLINE szind_t
rtree_leaf_elm_szind_read(tsdn_t *tsdn, rtree_t *rtree, rtree_leaf_elm_t *elm,
    bool acquired, bool dependent) {
	if (config_debug && acquired) {
		assert(dependent);
		rtree_leaf_elm_witness_access(tsdn, rtree, elm);
	}

#ifdef RTREE_LEAF_COMPACT
	uintptr_t bits = rtree_leaf_elm_bits_read(tsdn, rtree, elm, acquired,
	    dependent);
	assert(!acquired || rtree_leaf_elm_bits_locked_get(bits));
	return rtree_leaf_elm_bits_szind_get(bits);
#else
	return (szind_t)atomic_load_u(&elm->le_szind, dependent ? ATOMIC_RELAXED
	    : ATOMIC_ACQUIRE);
#endif
}

JEMALLOC_ALWAYS_INLINE bool
rtree_leaf_elm_slab_read(tsdn_t *tsdn, rtree_t *rtree, rtree_leaf_elm_t *elm,
    bool acquired, bool dependent) {
	if (config_debug && acquired) {
		assert(dependent);
		rtree_leaf_elm_witness_access(tsdn, rtree, elm);
	}

#ifdef RTREE_LEAF_COMPACT
	uintptr_t bits = rtree_leaf_elm_bits_read(tsdn, rtree, elm, acquired,
	    dependent);
	assert(!acquired || rtree_leaf_elm_bits_locked_get(bits));
	return rtree_leaf_elm_bits_slab_get(bits);
#else
	return atomic_load_b(&elm->le_slab, dependent ? ATOMIC_RELAXED :
	    ATOMIC_ACQUIRE);
#endif
}

JEMALLOC_INLINE void
rtree_leaf_elm_extent_write(tsdn_t *tsdn, rtree_t *rtree, rtree_leaf_elm_t *elm,
    bool acquired, extent_t *extent) {
	if (config_debug && acquired) {
		rtree_leaf_elm_witness_access(tsdn, rtree, elm);
	}
	assert(((uintptr_t)extent & (uintptr_t)0x1) == (uintptr_t)0x0);

#ifdef RTREE_LEAF_COMPACT
	uintptr_t old_bits = rtree_leaf_elm_bits_read(tsdn, rtree, elm,
	    acquired, acquired);
	uintptr_t bits = ((uintptr_t)rtree_leaf_elm_bits_szind_get(old_bits) <<
	    LG_VADDR) | ((uintptr_t)extent & (((uintptr_t)0x1 << LG_VADDR) - 1))
	    | ((uintptr_t)rtree_leaf_elm_bits_slab_get(old_bits) << 1) |
	    (uintptr_t)acquired;
	atomic_store_p(&elm->le_bits, (void *)bits, ATOMIC_RELEASE);
#else
	if (acquired) {
		/* Overlay lock bit. */
		extent = (extent_t *)((uintptr_t)extent | (uintptr_t)0x1);
	}
	atomic_store_p(&elm->le_extent, extent, ATOMIC_RELEASE);
#endif
}

JEMALLOC_INLINE void
rtree_leaf_elm_szind_write(tsdn_t *tsdn, rtree_t *rtree, rtree_leaf_elm_t *elm,
    bool acquired, szind_t szind) {
	if (config_debug && acquired) {
		rtree_leaf_elm_witness_access(tsdn, rtree, elm);
	}
	assert(szind <= NSIZES);

#ifdef RTREE_LEAF_COMPACT
	uintptr_t old_bits = rtree_leaf_elm_bits_read(tsdn, rtree, elm,
	    acquired, acquired);
	uintptr_t bits = ((uintptr_t)szind << LG_VADDR) |
	    ((uintptr_t)rtree_leaf_elm_bits_extent_get(old_bits) &
	    (((uintptr_t)0x1 << LG_VADDR) - 1)) |
	    ((uintptr_t)rtree_leaf_elm_bits_slab_get(old_bits) << 1) |
	    (uintptr_t)acquired;
	atomic_store_p(&elm->le_bits, (void *)bits, ATOMIC_RELEASE);
#else
	atomic_store_u(&elm->le_szind, szind, ATOMIC_RELEASE);
#endif
}

JEMALLOC_INLINE void
rtree_leaf_elm_slab_write(tsdn_t *tsdn, rtree_t *rtree, rtree_leaf_elm_t *elm,
     bool acquired, bool slab) {
	if (config_debug && acquired) {
		rtree_leaf_elm_witness_access(tsdn, rtree, elm);
	}

#ifdef RTREE_LEAF_COMPACT
	uintptr_t old_bits = rtree_leaf_elm_bits_read(tsdn, rtree, elm,
	    acquired, acquired);
	uintptr_t bits = ((uintptr_t)rtree_leaf_elm_bits_szind_get(old_bits) <<
	    LG_VADDR) | ((uintptr_t)rtree_leaf_elm_bits_extent_get(old_bits) &
	    (((uintptr_t)0x1 << LG_VADDR) - 1)) | ((uintptr_t)slab << 1) |
	    (uintptr_t)acquired;
	atomic_store_p(&elm->le_bits, (void *)bits, ATOMIC_RELEASE);
#else
	atomic_store_b(&elm->le_slab, slab, ATOMIC_RELEASE);
#endif
}

JEMALLOC_INLINE void
rtree_leaf_elm_write(tsdn_t *tsdn, rtree_t *rtree, rtree_leaf_elm_t *elm,
    bool acquired, extent_t *extent, szind_t szind, bool slab) {
	if (config_debug && acquired) {
		rtree_leaf_elm_witness_access(tsdn, rtree, elm);
	}
	assert(!slab || szind < NBINS);

#ifdef RTREE_LEAF_COMPACT
	uintptr_t bits = ((uintptr_t)szind << LG_VADDR) |
	    ((uintptr_t)extent & (((uintptr_t)0x1 << LG_VADDR) - 1)) |
	    ((uintptr_t)slab << 1) |
	    (uintptr_t)acquired;

	atomic_store_p(&elm->le_bits, (void *)bits, ATOMIC_RELEASE);
#else
	rtree_leaf_elm_slab_write(tsdn, rtree, elm, acquired, slab);
	rtree_leaf_elm_szind_write(tsdn, rtree, elm, acquired, szind);
	/*
	 * Write extent last, since the element is atomically considered valid
	 * as soon as the extent field is non-NULL.
	 */
	rtree_leaf_elm_extent_write(tsdn, rtree, elm, acquired, extent);
#endif
}

JEMALLOC_INLINE void
rtree_leaf_elm_szind_slab_update(tsdn_t *tsdn, rtree_t *rtree,
    rtree_leaf_elm_t *elm, szind_t szind, bool slab) {
	assert(!slab || szind < NBINS);

	/*
	 * The caller implicitly assures that it is the only writer to the szind
	 * and slab fields, and that the extent field cannot currently change.
	 */
#ifdef RTREE_LEAF_COMPACT
	/*
	 * Another thread may concurrently acquire the elm, which means that
	 * even though the szind and slab fields will not be concurrently
	 * modified by another thread, the fact that the lock is embedded in the
	 * same word requires that a CAS operation be used here.
	 */
	uintptr_t old_bits = rtree_leaf_elm_bits_read(tsdn, rtree, elm, false,
	    true) & ~((uintptr_t)0x1); /* Mask lock bit. */
	uintptr_t bits = ((uintptr_t)szind << LG_VADDR) |
	    ((uintptr_t)rtree_leaf_elm_bits_extent_get(old_bits) &
	    (((uintptr_t)0x1 << LG_VADDR) - 1)) |
	    ((uintptr_t)slab << 1);
	spin_t spinner = SPIN_INITIALIZER;
	while (true) {
		if (likely(atomic_compare_exchange_strong_p(&elm->le_bits,
		    (void **)&old_bits, (void *)bits, ATOMIC_ACQUIRE,
		    ATOMIC_RELAXED))) {
			break;
		}
		spin_adaptive(&spinner);
	}
#else
	/* No need to lock. */
	rtree_leaf_elm_slab_write(tsdn, rtree, elm, false, slab);
	rtree_leaf_elm_szind_write(tsdn, rtree, elm, false, szind);
#endif
}

JEMALLOC_ALWAYS_INLINE rtree_leaf_elm_t *
rtree_leaf_elm_lookup(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key, bool dependent, bool init_missing) {
	assert(key != 0);
	assert(!dependent || !init_missing);

	uintptr_t leafkey = rtree_leafkey(key);
	assert(leafkey != RTREE_LEAFKEY_INVALID);

#define RTREE_CACHE_CHECK(i) do {					\
	if (likely(rtree_ctx->cache[i].leafkey == leafkey)) {		\
		rtree_leaf_elm_t *leaf = rtree_ctx->cache[i].leaf;	\
		assert(leaf != NULL);					\
		if (i > 0) {						\
			/* Bubble up by one. */				\
			rtree_ctx->cache[i].leafkey =			\
				rtree_ctx->cache[i - 1].leafkey;	\
			rtree_ctx->cache[i].leaf =			\
				rtree_ctx->cache[i - 1].leaf;		\
			rtree_ctx->cache[i - 1].leafkey = leafkey;	\
			rtree_ctx->cache[i - 1].leaf = leaf;		\
		}							\
		uintptr_t subkey = rtree_subkey(key, RTREE_HEIGHT-1);	\
		return &leaf[subkey];					\
	}								\
} while (0)
	/* Check the first cache entry. */
	RTREE_CACHE_CHECK(0);
	/*
	 * Search the remaining cache elements, and on success move the matching
	 * element up by one slot.
	 */
	for (unsigned i = 1; i < RTREE_CTX_NCACHE; i++) {
		RTREE_CACHE_CHECK(i);
	}
#undef RTREE_CACHE_CHECK

	return rtree_leaf_elm_lookup_hard(tsdn, rtree, rtree_ctx, key,
	    dependent, init_missing);
}

JEMALLOC_INLINE bool
rtree_write(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx, uintptr_t key,
    extent_t *extent, szind_t szind, bool slab) {
	/* Use rtree_clear() to set the extent to NULL. */
	assert(extent != NULL);

	rtree_leaf_elm_t *elm = rtree_leaf_elm_lookup(tsdn, rtree, rtree_ctx,
	    key, false, true);
	if (elm == NULL) {
		return true;
	}

	assert(rtree_leaf_elm_extent_read(tsdn, rtree, elm, false, false) ==
	    NULL);
	rtree_leaf_elm_write(tsdn, rtree, elm, false, extent, szind, slab);

	return false;
}

JEMALLOC_ALWAYS_INLINE rtree_leaf_elm_t *
rtree_read(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx, uintptr_t key,
    bool dependent) {
	rtree_leaf_elm_t *elm = rtree_leaf_elm_lookup(tsdn, rtree, rtree_ctx,
	    key, dependent, false);
	if (!dependent && elm == NULL) {
		return NULL;
	}
	assert(elm != NULL);
	return elm;
}

JEMALLOC_ALWAYS_INLINE extent_t *
rtree_extent_read(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key, bool dependent) {
	rtree_leaf_elm_t *elm = rtree_read(tsdn, rtree, rtree_ctx, key,
	    dependent);
	if (!dependent && elm == NULL) {
		return NULL;
	}
	return rtree_leaf_elm_extent_read(tsdn, rtree, elm, false, dependent);
}

JEMALLOC_ALWAYS_INLINE szind_t
rtree_szind_read(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key, bool dependent) {
	rtree_leaf_elm_t *elm = rtree_read(tsdn, rtree, rtree_ctx, key,
	    dependent);
	if (!dependent && elm == NULL) {
		return NSIZES;
	}
	return rtree_leaf_elm_szind_read(tsdn, rtree, elm, false, dependent);
}

/*
 * rtree_slab_read() is intentionally omitted because slab is always read in
 * conjunction with szind, which makes rtree_szind_slab_read() a better choice.
 */

JEMALLOC_ALWAYS_INLINE bool
rtree_extent_szind_read(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key, bool dependent, extent_t **r_extent, szind_t *r_szind) {
	rtree_leaf_elm_t *elm = rtree_read(tsdn, rtree, rtree_ctx, key,
	    dependent);
	if (!dependent && elm == NULL) {
		return true;
	}
	*r_extent = rtree_leaf_elm_extent_read(tsdn, rtree, elm, false,
	    dependent);
	*r_szind = rtree_leaf_elm_szind_read(tsdn, rtree, elm, false,
	    dependent);
	return false;
}

JEMALLOC_ALWAYS_INLINE bool
rtree_szind_slab_read(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key, bool dependent, szind_t *r_szind, bool *r_slab) {
	rtree_leaf_elm_t *elm = rtree_read(tsdn, rtree, rtree_ctx, key,
	    dependent);
	if (!dependent && elm == NULL) {
		return true;
	}
	*r_szind = rtree_leaf_elm_szind_read(tsdn, rtree, elm, false,
	    dependent);
	*r_slab = rtree_leaf_elm_slab_read(tsdn, rtree, elm, false, dependent);
	return false;
}

JEMALLOC_INLINE rtree_leaf_elm_t *
rtree_leaf_elm_acquire(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key, bool dependent, bool init_missing) {
	rtree_leaf_elm_t *elm = rtree_leaf_elm_lookup(tsdn, rtree, rtree_ctx,
	    key, dependent, init_missing);
	if (!dependent && elm == NULL) {
		return NULL;
	}
	assert(elm != NULL);

	spin_t spinner = SPIN_INITIALIZER;
	while (true) {
		/* The least significant bit serves as a lock. */
#ifdef RTREE_LEAF_COMPACT
#  define RTREE_FIELD_WITH_LOCK le_bits
#else
#  define RTREE_FIELD_WITH_LOCK le_extent
#endif
		void *bits = atomic_load_p(&elm->RTREE_FIELD_WITH_LOCK,
		    ATOMIC_RELAXED);
		if (likely(((uintptr_t)bits & (uintptr_t)0x1) == 0)) {
			void *locked = (void *)((uintptr_t)bits |
			    (uintptr_t)0x1);
			if (likely(atomic_compare_exchange_strong_p(
			    &elm->RTREE_FIELD_WITH_LOCK, &bits, locked,
			    ATOMIC_ACQUIRE, ATOMIC_RELAXED))) {
				break;
			}
		}
		spin_adaptive(&spinner);
#undef RTREE_FIELD_WITH_LOCK
	}

	if (config_debug) {
		rtree_leaf_elm_witness_acquire(tsdn, rtree, key, elm);
	}

	return elm;
}

JEMALLOC_INLINE void
rtree_leaf_elm_release(tsdn_t *tsdn, rtree_t *rtree, rtree_leaf_elm_t *elm) {
	extent_t *extent = rtree_leaf_elm_extent_read(tsdn, rtree, elm, true,
	    true);
	rtree_leaf_elm_extent_write(tsdn, rtree, elm, false, extent);
	if (config_debug) {
		rtree_leaf_elm_witness_release(tsdn, rtree, elm);
	}
}

JEMALLOC_INLINE void
rtree_szind_slab_update(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key, szind_t szind, bool slab) {
	assert(!slab || szind < NBINS);

	rtree_leaf_elm_t *elm = rtree_read(tsdn, rtree, rtree_ctx, key, true);
	rtree_leaf_elm_szind_slab_update(tsdn, rtree, elm, szind, slab);
}

JEMALLOC_INLINE void
rtree_clear(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key) {
	rtree_leaf_elm_t *elm = rtree_read(tsdn, rtree, rtree_ctx, key, true);
	assert(rtree_leaf_elm_extent_read(tsdn, rtree, elm, false, false) !=
	    NULL);
	rtree_leaf_elm_write(tsdn, rtree, elm, false, NULL, NSIZES, false);
}
#endif

#endif /* JEMALLOC_INTERNAL_RTREE_INLINES_H */
