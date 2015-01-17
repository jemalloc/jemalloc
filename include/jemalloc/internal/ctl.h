/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

typedef struct ctl_node_s ctl_node_t;
typedef struct ctl_named_node_s ctl_named_node_t;
typedef struct ctl_indexed_node_s ctl_indexed_node_t;
typedef struct ctl_arena_stats_s ctl_arena_stats_t;
typedef struct ctl_stats_s ctl_stats_t;

#define	MCTLTYPE	0xf	/* Mask for the type */
typedef enum {
	MCTLTYPE_NODE	= 1,
	MCTLTYPE_INT	= 2,
	MCTLTYPE_STRING	= 3,
	MCTLTYPE_S64	= 4,
	MCTLTYPE_OPAQUE	= 5,
	MCTLTYPE_STRUCT	= MCTLTYPE_OPAQUE,
	MCTLTYPE_UINT	= 6,
	MCTLTYPE_LONG	= 7,
	MCTLTYPE_ULONG	= 8,
	MCTLTYPE_U64	= 9,
	/* Deviations from the FreeBSD kernel sysctl types: */
	MCTLTYPE_U32	= 0xb,
	MCTLTYPE_SIZE	= 0xc,		/* size_t and ssize_t */
	MCTLTYPE_SSIZE	= 0xd,
	MCTLTYPE_STRP	= 0xe,		/* Pointer to string */
	MCTLTYPE_BOOL	= 0xf,
} ctl_kind_t;

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

struct ctl_node_s {
	bool			named;
};

struct ctl_named_node_s {
	struct ctl_node_s	node;
	const char		*name;
	/* If (nchildren == 0), this is a terminal node. */
	unsigned		nchildren;
	const			ctl_node_t *children;
	int			(*ctl)(const size_t *, size_t, void *, size_t *,
	    void *, size_t);
	unsigned		kind;
};

struct ctl_indexed_node_s {
	struct ctl_node_s	node;
	const ctl_named_node_t	*(*index)(const size_t *, size_t, size_t);
};

struct ctl_arena_stats_s {
	bool			initialized;
	unsigned		nthreads;
	const char		*dss;
	size_t			pactive;
	size_t			pdirty;
	arena_stats_t		astats;

	/* Aggregate stats for small size classes, based on bin stats. */
	size_t			allocated_small;
	uint64_t		nmalloc_small;
	uint64_t		ndalloc_small;
	uint64_t		nrequests_small;

	malloc_bin_stats_t	bstats[NBINS];
	malloc_large_stats_t	*lstats;	/* nlclasses elements. */
	malloc_huge_stats_t	*hstats;	/* nhclasses elements. */
};

struct ctl_stats_s {
	size_t			allocated;
	size_t			active;
	size_t			mapped;
	struct {
		size_t		current;	/* stats_chunks.curchunks */
		uint64_t	total;		/* stats_chunks.nchunks */
		size_t		high;		/* stats_chunks.highchunks */
	} chunks;
	unsigned		narenas;
	ctl_arena_stats_t	*arenas;	/* (narenas + 1) elements. */
};

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

int	ctl_byname(const char *name, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen);
int	ctl_nametomib(const char *name, size_t *mibp, size_t *miblenp);

int	ctl_bymib(const size_t *mib, size_t miblen, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen);
bool	ctl_boot(void);
void	ctl_prefork(void);
void	ctl_postfork_parent(void);
void	ctl_postfork_child(void);

#define	xmallctl(name, oldp, oldlenp, newp, newlen) do {		\
	if (je_mallctl(name, oldp, oldlenp, newp, newlen)		\
	    != 0) {							\
		malloc_printf(						\
		    "<jemalloc>: Failure in xmallctl(\"%s\", ...)\n",	\
		    name);						\
		abort();						\
	}								\
} while (0)

#define	xmallctlnametomib(name, mibp, miblenp) do {			\
	if (je_mallctlnametomib(name, mibp, miblenp) != 0) {		\
		malloc_printf("<jemalloc>: Failure in "			\
		    "xmallctlnametomib(\"%s\", ...)\n", name);		\
		abort();						\
	}								\
} while (0)

#define	xmallctlbymib(mib, miblen, oldp, oldlenp, newp, newlen) do {	\
	if (je_mallctlbymib(mib, miblen, oldp, oldlenp, newp,		\
	    newlen) != 0) {						\
		malloc_write(						\
		    "<jemalloc>: Failure in xmallctlbymib()\n");	\
		abort();						\
	}								\
} while (0)

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/

