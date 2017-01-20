#ifndef JEMALLOC_INTERNAL_RTREE_INLINES_H
#define JEMALLOC_INTERNAL_RTREE_INLINES_H

#ifndef JEMALLOC_ENABLE_INLINE
unsigned	rtree_start_level(const rtree_t *rtree, uintptr_t key);
unsigned	rtree_ctx_start_level(const rtree_t *rtree,
    const rtree_ctx_t *rtree_ctx, uintptr_t key);
uintptr_t	rtree_subkey(rtree_t *rtree, uintptr_t key, unsigned level);

bool	rtree_node_valid(rtree_elm_t *node);
rtree_elm_t	*rtree_child_tryread(rtree_elm_t *elm, bool dependent);
rtree_elm_t	*rtree_child_read(tsdn_t *tsdn, rtree_t *rtree, rtree_elm_t *elm,
    unsigned level, bool dependent);
extent_t	*rtree_elm_read(rtree_elm_t *elm, bool dependent);
void	rtree_elm_write(rtree_elm_t *elm, const extent_t *extent);
rtree_elm_t	*rtree_subtree_tryread(rtree_t *rtree, unsigned level,
    bool dependent);
rtree_elm_t	*rtree_subtree_read(tsdn_t *tsdn, rtree_t *rtree,
    unsigned level, bool dependent);
rtree_elm_t	*rtree_elm_lookup(tsdn_t *tsdn, rtree_t *rtree,
    rtree_ctx_t *rtree_ctx, uintptr_t key, bool dependent, bool init_missing);

bool	rtree_write(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key, const extent_t *extent);
extent_t	*rtree_read(tsdn_t *tsdn, rtree_t *rtree,
    rtree_ctx_t *rtree_ctx, uintptr_t key, bool dependent);
rtree_elm_t	*rtree_elm_acquire(tsdn_t *tsdn, rtree_t *rtree,
    rtree_ctx_t *rtree_ctx, uintptr_t key, bool dependent, bool init_missing);
extent_t	*rtree_elm_read_acquired(tsdn_t *tsdn, const rtree_t *rtree,
    rtree_elm_t *elm);
void	rtree_elm_write_acquired(tsdn_t *tsdn, const rtree_t *rtree,
    rtree_elm_t *elm, const extent_t *extent);
void	rtree_elm_release(tsdn_t *tsdn, const rtree_t *rtree, rtree_elm_t *elm);
void	rtree_clear(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key);
#endif

#if (defined(JEMALLOC_ENABLE_INLINE) || defined(JEMALLOC_RTREE_C_))
JEMALLOC_ALWAYS_INLINE unsigned
rtree_start_level(const rtree_t *rtree, uintptr_t key) {
	unsigned start_level;

	if (unlikely(key == 0)) {
		return rtree->height - 1;
	}

	start_level = rtree->start_level[(lg_floor(key) + 1) >>
	    LG_RTREE_BITS_PER_LEVEL];
	assert(start_level < rtree->height);
	return start_level;
}

JEMALLOC_ALWAYS_INLINE unsigned
rtree_ctx_start_level(const rtree_t *rtree, const rtree_ctx_t *rtree_ctx,
    uintptr_t key) {
	unsigned start_level;
	uintptr_t key_diff;

	/* Compute the difference between old and new lookup keys. */
	key_diff = key ^ rtree_ctx->key;
	assert(key_diff != 0); /* Handled in rtree_elm_lookup(). */

	/*
	 * Compute the last traversal path element at which the keys' paths
	 * are the same.
	 */
	start_level = rtree->start_level[(lg_floor(key_diff) + 1) >>
	    LG_RTREE_BITS_PER_LEVEL];
	assert(start_level < rtree->height);
	return start_level;
}

JEMALLOC_ALWAYS_INLINE uintptr_t
rtree_subkey(rtree_t *rtree, uintptr_t key, unsigned level) {
	return ((key >> ((ZU(1) << (LG_SIZEOF_PTR+3)) -
	    rtree->levels[level].cumbits)) & ((ZU(1) <<
	    rtree->levels[level].bits) - 1));
}

JEMALLOC_ALWAYS_INLINE bool
rtree_node_valid(rtree_elm_t *node) {
	return ((uintptr_t)node != (uintptr_t)0);
}

JEMALLOC_ALWAYS_INLINE rtree_elm_t *
rtree_child_tryread(rtree_elm_t *elm, bool dependent) {
	rtree_elm_t *child;

	/* Double-checked read (first read may be stale). */
	child = elm->child;
	if (!dependent && !rtree_node_valid(child)) {
		child = (rtree_elm_t *)atomic_read_p(&elm->pun);
	}
	assert(!dependent || child != NULL);
	return child;
}

JEMALLOC_ALWAYS_INLINE rtree_elm_t *
rtree_child_read(tsdn_t *tsdn, rtree_t *rtree, rtree_elm_t *elm, unsigned level,
    bool dependent) {
	rtree_elm_t *child;

	child = rtree_child_tryread(elm, dependent);
	if (!dependent && unlikely(!rtree_node_valid(child))) {
		child = rtree_child_read_hard(tsdn, rtree, elm, level);
	}
	assert(!dependent || child != NULL);
	return child;
}

JEMALLOC_ALWAYS_INLINE extent_t *
rtree_elm_read(rtree_elm_t *elm, bool dependent) {
	extent_t *extent;

	if (dependent) {
		/*
		 * Reading a value on behalf of a pointer to a valid allocation
		 * is guaranteed to be a clean read even without
		 * synchronization, because the rtree update became visible in
		 * memory before the pointer came into existence.
		 */
		extent = elm->extent;
	} else {
		/*
		 * An arbitrary read, e.g. on behalf of ivsalloc(), may not be
		 * dependent on a previous rtree write, which means a stale read
		 * could result if synchronization were omitted here.
		 */
		extent = (extent_t *)atomic_read_p(&elm->pun);
	}

	/* Mask the lock bit. */
	extent = (extent_t *)((uintptr_t)extent & ~((uintptr_t)0x1));

	return extent;
}

JEMALLOC_INLINE void
rtree_elm_write(rtree_elm_t *elm, const extent_t *extent) {
	atomic_write_p(&elm->pun, extent);
}

JEMALLOC_ALWAYS_INLINE rtree_elm_t *
rtree_subtree_tryread(rtree_t *rtree, unsigned level, bool dependent) {
	rtree_elm_t *subtree;

	/* Double-checked read (first read may be stale). */
	subtree = rtree->levels[level].subtree;
	if (!dependent && unlikely(!rtree_node_valid(subtree))) {
		subtree = (rtree_elm_t *)atomic_read_p(
		    &rtree->levels[level].subtree_pun);
	}
	assert(!dependent || subtree != NULL);
	return subtree;
}

JEMALLOC_ALWAYS_INLINE rtree_elm_t *
rtree_subtree_read(tsdn_t *tsdn, rtree_t *rtree, unsigned level,
    bool dependent) {
	rtree_elm_t *subtree;

	subtree = rtree_subtree_tryread(rtree, level, dependent);
	if (!dependent && unlikely(!rtree_node_valid(subtree))) {
		subtree = rtree_subtree_read_hard(tsdn, rtree, level);
	}
	assert(!dependent || subtree != NULL);
	return subtree;
}

JEMALLOC_ALWAYS_INLINE rtree_elm_t *
rtree_elm_lookup(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key, bool dependent, bool init_missing) {
	uintptr_t subkey;
	unsigned start_level;
	rtree_elm_t *node;

	assert(!dependent || !init_missing);

	if (dependent || init_missing) {
		if (likely(rtree_ctx->valid)) {
			if (key == rtree_ctx->key) {
				return rtree_ctx->elms[rtree->height];
			} else {
				unsigned no_ctx_start_level =
				    rtree_start_level(rtree, key);
				unsigned ctx_start_level;

				if (likely(no_ctx_start_level <=
				    rtree_ctx->start_level && (ctx_start_level =
				    rtree_ctx_start_level(rtree, rtree_ctx,
				    key)) >= rtree_ctx->start_level)) {
					start_level = ctx_start_level;
					node = rtree_ctx->elms[ctx_start_level];
				} else {
					start_level = no_ctx_start_level;
					node = init_missing ?
					    rtree_subtree_read(tsdn, rtree,
					    no_ctx_start_level, dependent) :
					    rtree_subtree_tryread(rtree,
					    no_ctx_start_level, dependent);
					rtree_ctx->start_level =
					    no_ctx_start_level;
					rtree_ctx->elms[no_ctx_start_level] =
					    node;
				}
			}
		} else {
			unsigned no_ctx_start_level = rtree_start_level(rtree,
			    key);

			start_level = no_ctx_start_level;
			node = init_missing ? rtree_subtree_read(tsdn, rtree,
			    no_ctx_start_level, dependent) :
			    rtree_subtree_tryread(rtree, no_ctx_start_level,
			    dependent);
			rtree_ctx->valid = true;
			rtree_ctx->start_level = no_ctx_start_level;
			rtree_ctx->elms[no_ctx_start_level] = node;
		}
		rtree_ctx->key = key;
	} else {
		start_level = rtree_start_level(rtree, key);
		node = init_missing ? rtree_subtree_read(tsdn, rtree,
		    start_level, dependent) : rtree_subtree_tryread(rtree,
		    start_level, dependent);
	}

#define RTREE_GET_BIAS	(RTREE_HEIGHT_MAX - rtree->height)
	switch (start_level + RTREE_GET_BIAS) {
#define RTREE_GET_SUBTREE(level)					\
	case level:							\
		assert(level < (RTREE_HEIGHT_MAX-1));			\
		if (!dependent && unlikely(!rtree_node_valid(node))) {	\
			if (init_missing) {				\
				rtree_ctx->valid = false;		\
			}						\
			return NULL;					\
		}							\
		subkey = rtree_subkey(rtree, key, level -		\
		    RTREE_GET_BIAS);					\
		node = init_missing ? rtree_child_read(tsdn, rtree,	\
		    &node[subkey], level - RTREE_GET_BIAS, dependent) :	\
		    rtree_child_tryread(&node[subkey], dependent);	\
		if (dependent || init_missing) {			\
			rtree_ctx->elms[level - RTREE_GET_BIAS + 1] =	\
			    node;					\
		}							\
		/* Fall through. */
#define RTREE_GET_LEAF(level)						\
	case level:							\
		assert(level == (RTREE_HEIGHT_MAX-1));			\
		if (!dependent && unlikely(!rtree_node_valid(node))) {	\
			if (init_missing) {				\
				rtree_ctx->valid = false;		\
			}						\
			return NULL;					\
		}							\
		subkey = rtree_subkey(rtree, key, level -		\
		    RTREE_GET_BIAS);					\
		/*							\
		 * node is a leaf, so it contains values rather than	\
		 * child pointers.					\
		 */							\
		node = &node[subkey];					\
		if (dependent || init_missing) {			\
			rtree_ctx->elms[level - RTREE_GET_BIAS + 1] =	\
			    node;					\
		}							\
		return node;
#if RTREE_HEIGHT_MAX > 1
	RTREE_GET_SUBTREE(0)
#endif
#if RTREE_HEIGHT_MAX > 2
	RTREE_GET_SUBTREE(1)
#endif
#if RTREE_HEIGHT_MAX > 3
	RTREE_GET_SUBTREE(2)
#endif
#if RTREE_HEIGHT_MAX > 4
	RTREE_GET_SUBTREE(3)
#endif
#if RTREE_HEIGHT_MAX > 5
	RTREE_GET_SUBTREE(4)
#endif
#if RTREE_HEIGHT_MAX > 6
	RTREE_GET_SUBTREE(5)
#endif
#if RTREE_HEIGHT_MAX > 7
	RTREE_GET_SUBTREE(6)
#endif
#if RTREE_HEIGHT_MAX > 8
	RTREE_GET_SUBTREE(7)
#endif
#if RTREE_HEIGHT_MAX > 9
	RTREE_GET_SUBTREE(8)
#endif
#if RTREE_HEIGHT_MAX > 10
	RTREE_GET_SUBTREE(9)
#endif
#if RTREE_HEIGHT_MAX > 11
	RTREE_GET_SUBTREE(10)
#endif
#if RTREE_HEIGHT_MAX > 12
	RTREE_GET_SUBTREE(11)
#endif
#if RTREE_HEIGHT_MAX > 13
	RTREE_GET_SUBTREE(12)
#endif
#if RTREE_HEIGHT_MAX > 14
	RTREE_GET_SUBTREE(13)
#endif
#if RTREE_HEIGHT_MAX > 15
	RTREE_GET_SUBTREE(14)
#endif
#if RTREE_HEIGHT_MAX > 16
#  error Unsupported RTREE_HEIGHT_MAX
#endif
	RTREE_GET_LEAF(RTREE_HEIGHT_MAX-1)
#undef RTREE_GET_SUBTREE
#undef RTREE_GET_LEAF
	default: not_reached();
	}
#undef RTREE_GET_BIAS
	not_reached();
}

JEMALLOC_INLINE bool
rtree_write(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx, uintptr_t key,
    const extent_t *extent) {
	rtree_elm_t *elm;

	assert(extent != NULL); /* Use rtree_clear() for this case. */
	assert(((uintptr_t)extent & (uintptr_t)0x1) == (uintptr_t)0x0);

	elm = rtree_elm_lookup(tsdn, rtree, rtree_ctx, key, false, true);
	if (elm == NULL) {
		return true;
	}
	assert(rtree_elm_read(elm, false) == NULL);
	rtree_elm_write(elm, extent);

	return false;
}

JEMALLOC_ALWAYS_INLINE extent_t *
rtree_read(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx, uintptr_t key,
    bool dependent) {
	rtree_elm_t *elm;

	elm = rtree_elm_lookup(tsdn, rtree, rtree_ctx, key, dependent, false);
	if (elm == NULL) {
		return NULL;
	}

	return rtree_elm_read(elm, dependent);
}

JEMALLOC_INLINE rtree_elm_t *
rtree_elm_acquire(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key, bool dependent, bool init_missing) {
	rtree_elm_t *elm;

	elm = rtree_elm_lookup(tsdn, rtree, rtree_ctx, key, dependent,
	    init_missing);
	if (!dependent && elm == NULL) {
		return NULL;
	}
	{
		extent_t *extent;
		void *s;

		do {
			extent = rtree_elm_read(elm, false);
			/* The least significant bit serves as a lock. */
			s = (void *)((uintptr_t)extent | (uintptr_t)0x1);
		} while (atomic_cas_p(&elm->pun, (void *)extent, s));
	}

	if (config_debug) {
		rtree_elm_witness_acquire(tsdn, rtree, key, elm);
	}

	return elm;
}

JEMALLOC_INLINE extent_t *
rtree_elm_read_acquired(tsdn_t *tsdn, const rtree_t *rtree, rtree_elm_t *elm) {
	extent_t *extent;

	assert(((uintptr_t)elm->pun & (uintptr_t)0x1) == (uintptr_t)0x1);
	extent = (extent_t *)((uintptr_t)elm->pun & ~((uintptr_t)0x1));
	assert(((uintptr_t)extent & (uintptr_t)0x1) == (uintptr_t)0x0);

	if (config_debug) {
		rtree_elm_witness_access(tsdn, rtree, elm);
	}

	return extent;
}

JEMALLOC_INLINE void
rtree_elm_write_acquired(tsdn_t *tsdn, const rtree_t *rtree, rtree_elm_t *elm,
    const extent_t *extent) {
	assert(((uintptr_t)extent & (uintptr_t)0x1) == (uintptr_t)0x0);
	assert(((uintptr_t)elm->pun & (uintptr_t)0x1) == (uintptr_t)0x1);

	if (config_debug) {
		rtree_elm_witness_access(tsdn, rtree, elm);
	}

	elm->pun = (void *)((uintptr_t)extent | (uintptr_t)0x1);
	assert(rtree_elm_read_acquired(tsdn, rtree, elm) == extent);
}

JEMALLOC_INLINE void
rtree_elm_release(tsdn_t *tsdn, const rtree_t *rtree, rtree_elm_t *elm) {
	rtree_elm_write(elm, rtree_elm_read_acquired(tsdn, rtree, elm));
	if (config_debug) {
		rtree_elm_witness_release(tsdn, rtree, elm);
	}
}

JEMALLOC_INLINE void
rtree_clear(tsdn_t *tsdn, rtree_t *rtree, rtree_ctx_t *rtree_ctx,
    uintptr_t key) {
	rtree_elm_t *elm;

	elm = rtree_elm_acquire(tsdn, rtree, rtree_ctx, key, true, false);
	rtree_elm_write_acquired(tsdn, rtree, elm, NULL);
	rtree_elm_release(tsdn, rtree, elm);
}
#endif

#endif /* JEMALLOC_INTERNAL_RTREE_INLINES_H */
