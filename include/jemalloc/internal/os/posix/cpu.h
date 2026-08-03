#ifndef JEMALLOC_INTERNAL_OS_POSIX_CPU_H
#define JEMALLOC_INTERNAL_OS_POSIX_CPU_H

#include "jemalloc/internal/jemalloc_preamble.h"

JEMALLOC_ALWAYS_INLINE unsigned
os_cpu_ncpus(void) {
	long result;

#ifdef CPU_COUNT
	/*
	 * glibc >= 2.6 has the CPU_COUNT macro.
	 *
	 * glibc's sysconf() uses isspace().  glibc allocates for the first time
	 * *before* setting up the isspace tables.  Therefore we need a
	 * different method to get the number of CPUs.
	 *
	 * The getaffinity approach is also preferred when only a subset of CPUs
	 * is available, to avoid using more arenas than necessary.
	 */
	{
#	if defined(__FreeBSD__) || defined(__DragonFly__)
		cpuset_t set;
#	else
		cpu_set_t set;
#	endif
#	if defined(JEMALLOC_HAVE_SCHED_SETAFFINITY)
		sched_getaffinity(0, sizeof(set), &set);
#	else
		pthread_getaffinity_np(pthread_self(), sizeof(set), &set);
#	endif
		result = CPU_COUNT(&set);
	}
#else
	result = sysconf(_SC_NPROCESSORS_ONLN);
#endif
	return ((result == -1) ? 1 : (unsigned)result);
}

/*
 * Ensure that number of CPUs is determistinc, i.e. it is the same based on:
 * - sched_getaffinity()
 * - _SC_NPROCESSORS_ONLN
 * - _SC_NPROCESSORS_CONF
 * Since otherwise tricky things is possible with percpu arenas in use.
 */
JEMALLOC_ALWAYS_INLINE bool
os_cpu_count_is_deterministic(void) {
	long cpu_onln = sysconf(_SC_NPROCESSORS_ONLN);
	long cpu_conf = sysconf(_SC_NPROCESSORS_CONF);
	if (cpu_onln != cpu_conf) {
		return false;
	}
#	if defined(CPU_COUNT)
#		if defined(__FreeBSD__) || defined(__DragonFly__)
	cpuset_t set;
#		else
	cpu_set_t set;
#		endif /* __FreeBSD__ */
#		if defined(JEMALLOC_HAVE_SCHED_SETAFFINITY)
	sched_getaffinity(0, sizeof(set), &set);
#		else  /* !JEMALLOC_HAVE_SCHED_SETAFFINITY */
	pthread_getaffinity_np(pthread_self(), sizeof(set), &set);
#		endif /* JEMALLOC_HAVE_SCHED_SETAFFINITY */
	long cpu_affinity = CPU_COUNT(&set);
	if (cpu_affinity != cpu_conf) {
		return false;
	}
#	endif         /* CPU_COUNT */
	return true;
}

JEMALLOC_ALWAYS_INLINE int
os_cpu_current(void) {
#if defined(JEMALLOC_HAVE_SCHED_GETCPU)
	return sched_getcpu();
#elif defined(JEMALLOC_HAVE_RDTSCP)
	unsigned int ecx;
	asm volatile("rdtscp" : "=c"(ecx)::"eax", "edx");
	return (int)(ecx & 0xfff);
#else
	not_reached();
	return -1;
#endif
}

JEMALLOC_ALWAYS_INLINE bool
os_cpu_set_affinity(int cpu) {
#if defined(JEMALLOC_HAVE_SCHED_SETAFFINITY)                                  \
    || defined(JEMALLOC_HAVE_PTHREAD_SETAFFINITY_NP)
#	if defined(JEMALLOC_HAVE_SCHED_SETAFFINITY)
	cpu_set_t cpuset;
#	else
#		ifndef __NetBSD__
	cpuset_t cpuset;
#		else
	cpuset_t *cpuset;
#		endif
#	endif

#	ifndef __NetBSD__
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);
#	else
	cpuset = cpuset_create();
#	endif

#	if defined(JEMALLOC_HAVE_SCHED_SETAFFINITY)
	return (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0);
#	else
#		ifndef __NetBSD__
	int ret = pthread_setaffinity_np(
	    pthread_self(), sizeof(cpuset_t), &cpuset);
#		else
	int ret = pthread_setaffinity_np(
	    pthread_self(), cpuset_size(cpuset), cpuset);
	cpuset_destroy(cpuset);
#		endif
	return ret != 0;
#	endif
#else
	return false;
#endif
}

#endif /* JEMALLOC_INTERNAL_OS_POSIX_CPU_H */
