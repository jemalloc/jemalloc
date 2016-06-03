typedef struct extent_hooks_s extent_hooks_t;

/*
 * void *
 * extent_alloc(void *new_addr, size_t size, size_t alignment, bool *zero,
 *     bool *commit, unsigned arena_ind);
 */
typedef void *(extent_alloc_t)(extent_hooks_t *, void *, size_t, size_t, bool *,
    bool *, unsigned);

/*
 * bool
 * extent_dalloc(void *addr, size_t size, bool committed, unsigned arena_ind);
 */
typedef bool (extent_dalloc_t)(extent_hooks_t *, void *, size_t, bool,
    unsigned);

/*
 * bool
 * extent_commit(void *addr, size_t size, size_t offset, size_t length,
 *     unsigned arena_ind);
 */
typedef bool (extent_commit_t)(extent_hooks_t *, void *, size_t, size_t, size_t,
    unsigned);

/*
 * bool
 * extent_decommit(void *addr, size_t size, size_t offset, size_t length,
 *     unsigned arena_ind);
 */
typedef bool (extent_decommit_t)(extent_hooks_t *, void *, size_t, size_t,
    size_t, unsigned);

/*
 * bool
 * extent_purge(void *addr, size_t size, size_t offset, size_t length,
 *     unsigned arena_ind);
 */
typedef bool (extent_purge_t)(extent_hooks_t *, void *, size_t, size_t, size_t,
    unsigned);

/*
 * bool
 * extent_split(void *addr, size_t size, size_t size_a, size_t size_b,
 *     bool committed, unsigned arena_ind);
 */
typedef bool (extent_split_t)(extent_hooks_t *, void *, size_t, size_t, size_t,
    bool, unsigned);

/*
 * bool
 * extent_merge(void *addr_a, size_t size_a, void *addr_b, size_t size_b,
 *     bool committed, unsigned arena_ind);
 */
typedef bool (extent_merge_t)(extent_hooks_t *, void *, size_t, void *, size_t,
    bool, unsigned);

struct extent_hooks_s {
	extent_alloc_t		*alloc;
	extent_dalloc_t		*dalloc;
	extent_commit_t		*commit;
	extent_decommit_t	*decommit;
	extent_purge_t		*purge;
	extent_split_t		*split;
	extent_merge_t		*merge;
};
