#ifndef JEMALLOC_INTERNAL_CTL_EXTERNS_H
#define JEMALLOC_INTERNAL_CTL_EXTERNS_H

/* Maximum ctl tree depth. */
#define CTL_MAX_DEPTH	7

#define NUM_GLOBAL_PROF_MUTEXES	3
#define NUM_ARENA_PROF_MUTEXES	6
#define NUM_MUTEX_PROF_COUNTERS	7

extern const char *arena_mutex_names[NUM_ARENA_PROF_MUTEXES];
extern const char *global_mutex_names[NUM_GLOBAL_PROF_MUTEXES];
extern const char *mutex_counter_names[NUM_MUTEX_PROF_COUNTERS];

int	ctl_byname(tsd_t *tsd, const char *name, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen);
int	ctl_nametomib(tsdn_t *tsdn, const char *name, size_t *mibp,
    size_t *miblenp);

int	ctl_bymib(tsd_t *tsd, const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen);
bool	ctl_boot(void);
void	ctl_prefork(tsdn_t *tsdn);
void	ctl_postfork_parent(tsdn_t *tsdn);
void	ctl_postfork_child(tsdn_t *tsdn);

#define xmallctl(name, oldp, oldlenp, newp, newlen) do {		\
	if (je_mallctl(name, oldp, oldlenp, newp, newlen)		\
	    != 0) {							\
		malloc_printf(						\
		    "<jemalloc>: Failure in xmallctl(\"%s\", ...)\n",	\
		    name);						\
		abort();						\
	}								\
} while (0)

#define xmallctlnametomib(name, mibp, miblenp) do {			\
	if (je_mallctlnametomib(name, mibp, miblenp) != 0) {		\
		malloc_printf("<jemalloc>: Failure in "			\
		    "xmallctlnametomib(\"%s\", ...)\n", name);		\
		abort();						\
	}								\
} while (0)

#define xmallctlbymib(mib, miblen, oldp, oldlenp, newp, newlen) do {	\
	if (je_mallctlbymib(mib, miblen, oldp, oldlenp, newp,		\
	    newlen) != 0) {						\
		malloc_write(						\
		    "<jemalloc>: Failure in xmallctlbymib()\n");	\
		abort();						\
	}								\
} while (0)

#endif /* JEMALLOC_INTERNAL_CTL_EXTERNS_H */
