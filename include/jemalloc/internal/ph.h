#ifndef JEMALLOC_INTERNAL_PH_H
#define JEMALLOC_INTERNAL_PH_H

/*
 * A Pairing Heap implementation.
 *
 * "The Pairing Heap: A New Form of Self-Adjusting Heap"
 * https://www.cs.cmu.edu/~sleator/papers/pairing-heaps.pdf
 *
 * With auxiliary twopass list, described in a follow on paper.
 *
 * "Pairing Heaps: Experiments and Analysis"
 * http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.106.2988&rep=rep1&type=pdf
 *
 *******************************************************************************
 */

typedef int (*ph_cmp_t)(void *, void *);

/* Node structure. */
typedef struct phn_link_s phn_link_t;
struct phn_link_s {
	void *prev;
	void *next;
	void *lchild;
};

typedef struct ph_s ph_t;
struct ph_s {
	void *root;
};

JEMALLOC_ALWAYS_INLINE phn_link_t *
phn_link_get(void *phn, size_t offset) {
	return (phn_link_t *)(((uintptr_t)phn) + offset);
}

JEMALLOC_ALWAYS_INLINE void
phn_link_init(void *phn, size_t offset) {
	phn_link_get(phn, offset)->prev = NULL;
	phn_link_get(phn, offset)->next = NULL;
	phn_link_get(phn, offset)->lchild = NULL;
}

/* Internal utility helpers. */
JEMALLOC_ALWAYS_INLINE void *
phn_lchild_get(void *phn, size_t offset) {
	return phn_link_get(phn, offset)->lchild;
}

JEMALLOC_ALWAYS_INLINE void
phn_lchild_set(void *phn, void *lchild, size_t offset) {
	phn_link_get(phn, offset)->lchild = lchild;
}

JEMALLOC_ALWAYS_INLINE void *
phn_next_get(void *phn, size_t offset) {
	return phn_link_get(phn, offset)->next;
}

JEMALLOC_ALWAYS_INLINE void
phn_next_set(void *phn, void *next, size_t offset) {
	phn_link_get(phn, offset)->next = next;
}

JEMALLOC_ALWAYS_INLINE void *
phn_prev_get(void *phn, size_t offset) {
	return phn_link_get(phn, offset)->prev;
}

JEMALLOC_ALWAYS_INLINE void
phn_prev_set(void *phn, void *prev, size_t offset) {
	phn_link_get(phn, offset)->prev = prev;
}

JEMALLOC_ALWAYS_INLINE void
phn_merge_ordered(void *phn0, void *phn1, size_t offset,
    ph_cmp_t cmp) {
	void *phn0child;

	assert(phn0 != NULL);
	assert(phn1 != NULL);
	assert(cmp(phn0, phn1) <= 0);

	phn_prev_set(phn1, phn0, offset);
	phn0child = phn_lchild_get(phn0, offset);
	phn_next_set(phn1, phn0child, offset);
	if (phn0child != NULL) {
		phn_prev_set(phn0child, phn1, offset);
	}
	phn_lchild_set(phn0, phn1, offset);
}

JEMALLOC_ALWAYS_INLINE void *
phn_merge(void *phn0, void *phn1, size_t offset, ph_cmp_t cmp) {
	void *result;
	if (phn0 == NULL) {
		result = phn1;
	} else if (phn1 == NULL) {
		result = phn0;
	} else if (cmp(phn0, phn1) < 0) {
		phn_merge_ordered(phn0, phn1, offset, cmp);
		result = phn0;
	} else {
		phn_merge_ordered(phn1, phn0, offset, cmp);
		result = phn1;
	}
	return result;
}

JEMALLOC_ALWAYS_INLINE void *
phn_merge_siblings(void *phn, size_t offset, ph_cmp_t cmp) {
	void *head = NULL;
	void *tail = NULL;
	void *phn0 = phn;
	void *phn1 = phn_next_get(phn0, offset);

	/*
	 * Multipass merge, wherein the first two elements of a FIFO
	 * are repeatedly merged, and each result is appended to the
	 * singly linked FIFO, until the FIFO contains only a single
	 * element.  We start with a sibling list but no reference to
	 * its tail, so we do a single pass over the sibling list to
	 * populate the FIFO.
	 */
	if (phn1 != NULL) {
		void *phnrest = phn_next_get(phn1, offset);
		if (phnrest != NULL) {
			phn_prev_set(phnrest, NULL, offset);
		}
		phn_prev_set(phn0, NULL, offset);
		phn_next_set(phn0, NULL, offset);
		phn_prev_set(phn1, NULL, offset);
		phn_next_set(phn1, NULL, offset);
		phn0 = phn_merge(phn0, phn1, offset, cmp);
		head = tail = phn0;
		phn0 = phnrest;
		while (phn0 != NULL) {
			phn1 = phn_next_get(phn0, offset);
			if (phn1 != NULL) {
				phnrest = phn_next_get(phn1, offset);
				if (phnrest != NULL) {
					phn_prev_set(phnrest, NULL, offset);
				}
				phn_prev_set(phn0, NULL, offset);
				phn_next_set(phn0, NULL, offset);
				phn_prev_set(phn1, NULL, offset);
				phn_next_set(phn1, NULL, offset);
				phn0 = phn_merge(phn0, phn1, offset, cmp);
				phn_next_set(tail, phn0, offset);
				tail = phn0;
				phn0 = phnrest;
			} else {
				phn_next_set(tail, phn0, offset);
				tail = phn0;
				phn0 = NULL;
			}
		}
		phn0 = head;
		phn1 = phn_next_get(phn0, offset);
		if (phn1 != NULL) {
			while (true) {
				head = phn_next_get(phn1, offset);
				assert(phn_prev_get(phn0, offset) == NULL);
				phn_next_set(phn0, NULL, offset);
				assert(phn_prev_get(phn1, offset) == NULL);
				phn_next_set(phn1, NULL, offset);
				phn0 = phn_merge(phn0, phn1, offset, cmp);
				if (head == NULL) {
					break;
				}
				phn_next_set(tail, phn0, offset);
				tail = phn0;
				phn0 = head;
				phn1 = phn_next_get(phn0, offset);
			}
		}
	}
	return phn0;
}

JEMALLOC_ALWAYS_INLINE void
ph_merge_aux(ph_t *ph, size_t offset, ph_cmp_t cmp) {
	void *phn = phn_next_get(ph->root, offset);
	if (phn != NULL) {
		phn_prev_set(ph->root, NULL, offset);
		phn_next_set(ph->root, NULL, offset);
		phn_prev_set(phn, NULL, offset);
		phn = phn_merge_siblings(phn, offset, cmp);
		assert(phn_next_get(phn, offset) == NULL);
		ph->root = phn_merge(ph->root, phn, offset, cmp);
	}
}

JEMALLOC_ALWAYS_INLINE void *
ph_merge_children(void *phn, size_t offset, ph_cmp_t cmp) {
	void *result;
	void *lchild = phn_lchild_get(phn, offset);
	if (lchild == NULL) {
		result = NULL;
	} else {
		result = phn_merge_siblings(lchild, offset, cmp);
	}
	return result;
}

JEMALLOC_ALWAYS_INLINE void
ph_new(ph_t *ph) {
	ph->root = NULL;
}

JEMALLOC_ALWAYS_INLINE bool
ph_empty(ph_t *ph) {
	return ph->root == NULL;
}

JEMALLOC_ALWAYS_INLINE void *
ph_first(ph_t *ph, size_t offset, ph_cmp_t cmp) {
	if (ph->root == NULL) {
		return NULL;
	}
	ph_merge_aux(ph, offset, cmp);
	return ph->root;
}

JEMALLOC_ALWAYS_INLINE void *
ph_any(ph_t *ph, size_t offset) {
	if (ph->root == NULL) {
		return NULL;
	}
	void *aux = phn_next_get(ph->root, offset);
	if (aux != NULL) {
		return aux;
	}
	return ph->root;
}

JEMALLOC_ALWAYS_INLINE void
ph_insert(ph_t *ph, void *phn, size_t offset) {
	phn_link_init(phn, offset);

	/*
	 * Treat the root as an aux list during insertion, and lazily merge
	 * during a_prefix##remove_first().  For elements that are inserted,
	 * then removed via a_prefix##remove() before the aux list is ever
	 * processed, this makes insert/remove constant-time, whereas eager
	 * merging would make insert O(log n).
	 */
	if (ph->root == NULL) {
		ph->root = phn;
	} else {
		phn_next_set(phn, phn_next_get(ph->root, offset), offset);
		if (phn_next_get(ph->root, offset) != NULL) {
			phn_prev_set(phn_next_get(ph->root, offset), phn,
			    offset);
		}
		phn_prev_set(phn, ph->root, offset);
		phn_next_set(ph->root, phn, offset);
	}
}

JEMALLOC_ALWAYS_INLINE void *
ph_remove_first(ph_t *ph, size_t offset, ph_cmp_t cmp) {
	void *ret;

	if (ph->root == NULL) {
		return NULL;
	}
	ph_merge_aux(ph, offset, cmp);
	ret = ph->root;
	ph->root = ph_merge_children(ph->root, offset, cmp);

	return ret;

}

JEMALLOC_ALWAYS_INLINE void *
ph_remove_any(ph_t *ph, size_t offset, ph_cmp_t cmp) {
	/*
	 * Remove the most recently inserted aux list element, or the root if
	 * the aux list is empty.  This has the effect of behaving as a LIFO
	 * (and insertion/removal is therefore constant-time) if
	 * a_prefix##[remove_]first() are never called.
	 */
	if (ph->root == NULL) {
		return NULL;
	}
	void *ret = phn_next_get(ph->root, offset);
	if (ret != NULL) {
		void *aux = phn_next_get(ret, offset);
		phn_next_set(ph->root, aux, offset);
		if (aux != NULL) {
			phn_prev_set(aux, ph->root, offset);
		}
		return ret;
	}
	ret = ph->root;
	ph->root = ph_merge_children(ph->root, offset, cmp);
	return ret;
}

JEMALLOC_ALWAYS_INLINE void
ph_remove(ph_t *ph, void *phn, size_t offset, ph_cmp_t cmp) {
	void *replace;
	void *parent;

	if (ph->root == phn) {
		/*
		 * We can delete from aux list without merging it, but we need
		 * to merge if we are dealing with the root node and it has
		 * children.
		 */
		if (phn_lchild_get(phn, offset) == NULL) {
			ph->root = phn_next_get(phn, offset);
			if (ph->root != NULL) {
				phn_prev_set(ph->root, NULL, offset);
			}
			return;
		}
		ph_merge_aux(ph, offset, cmp);
		if (ph->root == phn) {
			ph->root = ph_merge_children(ph->root, offset, cmp);
			return;
		}
	}

	/* Get parent (if phn is leftmost child) before mutating. */
	if ((parent = phn_prev_get(phn, offset)) != NULL) {
		if (phn_lchild_get(parent, offset) != phn) {
			parent = NULL;
		}
	}
	/* Find a possible replacement node, and link to parent. */
	replace = ph_merge_children(phn, offset, cmp);
	/* Set next/prev for sibling linked list. */
	if (replace != NULL) {
		if (parent != NULL) {
			phn_prev_set(replace, parent, offset);
			phn_lchild_set(parent, replace, offset);
		} else {
			phn_prev_set(replace, phn_prev_get(phn, offset),
			    offset);
			if (phn_prev_get(phn, offset) != NULL) {
				phn_next_set(phn_prev_get(phn, offset), replace,
				    offset);
			}
		}
		phn_next_set(replace, phn_next_get(phn, offset), offset);
		if (phn_next_get(phn, offset) != NULL) {
			phn_prev_set(phn_next_get(phn, offset), replace,
			    offset);
		}
	} else {
		if (parent != NULL) {
			void *next = phn_next_get(phn, offset);
			phn_lchild_set(parent, next, offset);
			if (next != NULL) {
				phn_prev_set(next, parent, offset);
			}
		} else {
			assert(phn_prev_get(phn, offset) != NULL);
			phn_next_set(
			    phn_prev_get(phn, offset),
			    phn_next_get(phn, offset), offset);
		}
		if (phn_next_get(phn, offset) != NULL) {
			phn_prev_set(
			    phn_next_get(phn, offset),
			    phn_prev_get(phn, offset), offset);
		}
	}
}

#define ph_structs(a_prefix, a_type)					\
typedef struct {							\
	phn_link_t link;						\
} a_prefix##_link_t;							\
									\
typedef struct {							\
	ph_t ph;							\
} a_prefix##_t;

/*
 * The ph_proto() macro generates function prototypes that correspond to the
 * functions generated by an equivalently parameterized call to ph_gen().
 */
#define ph_proto(a_attr, a_prefix, a_type)				\
									\
a_attr void a_prefix##_new(a_prefix##_t *ph);				\
a_attr bool a_prefix##_empty(a_prefix##_t *ph);				\
a_attr a_type *a_prefix##_first(a_prefix##_t *ph);			\
a_attr a_type *a_prefix##_any(a_prefix##_t *ph);			\
a_attr void a_prefix##_insert(a_prefix##_t *ph, a_type *phn);		\
a_attr a_type *a_prefix##_remove_first(a_prefix##_t *ph);		\
a_attr a_type *a_prefix##_remove_any(a_prefix##_t *ph);			\
a_attr void a_prefix##_remove(a_prefix##_t *ph, a_type *phn);

/* The ph_gen() macro generates a type-specific pairing heap implementation. */
#define ph_gen(a_attr, a_prefix, a_type, a_field, a_cmp)		\
JEMALLOC_ALWAYS_INLINE int						\
a_prefix##_ph_cmp(void *a, void *b) {					\
	return a_cmp((a_type *)a, (a_type *)b);				\
}									\
									\
a_attr void								\
a_prefix##_new(a_prefix##_t *ph) {					\
	ph_new(&ph->ph);						\
}									\
									\
a_attr bool								\
a_prefix##_empty(a_prefix##_t *ph) {					\
	return ph_empty(&ph->ph);					\
}									\
									\
a_attr a_type *								\
a_prefix##_first(a_prefix##_t *ph) {					\
	return ph_first(&ph->ph, offsetof(a_type, a_field),		\
	    &a_prefix##_ph_cmp);					\
}									\
									\
a_attr a_type *								\
a_prefix##_any(a_prefix##_t *ph) {					\
	return ph_any(&ph->ph, offsetof(a_type, a_field));		\
}									\
									\
a_attr void								\
a_prefix##_insert(a_prefix##_t *ph, a_type *phn) {			\
	ph_insert(&ph->ph, phn, offsetof(a_type, a_field));		\
}									\
									\
a_attr a_type *								\
a_prefix##_remove_first(a_prefix##_t *ph) {				\
	return ph_remove_first(&ph->ph, offsetof(a_type, a_field),	\
	    a_prefix##_ph_cmp);						\
}									\
									\
a_attr a_type *								\
a_prefix##_remove_any(a_prefix##_t *ph) {				\
	return ph_remove_any(&ph->ph, offsetof(a_type, a_field),	\
	    a_prefix##_ph_cmp);						\
}									\
									\
a_attr void								\
a_prefix##_remove(a_prefix##_t *ph, a_type *phn) {			\
	ph_remove(&ph->ph, phn, offsetof(a_type, a_field),		\
	    a_prefix##_ph_cmp);						\
}

#endif /* JEMALLOC_INTERNAL_PH_H */
