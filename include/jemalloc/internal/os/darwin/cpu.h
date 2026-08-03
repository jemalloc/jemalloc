#ifndef JEMALLOC_INTERNAL_OS_DARWIN_CPU_H
#define JEMALLOC_INTERNAL_OS_DARWIN_CPU_H

/*
 * Darwin CPU backend. os_cpu_ncpus()/os_cpu_count_is_deterministic() are
 * identical to posix/cpu.h's (macOS has no CPU_COUNT/sched_getaffinity()
 * either, so both already fall through to the same sysconf() path) --
 * duplicated here rather than shared via #include, matching every other
 * os/<os>/<module>.h backend (each is self-contained; see os/darwin/mutex.h).
 * os_cpu_current() is genuinely different: no sched_getcpu() on macOS, so it
 * reads the CPU index directly out of a CPU register instead.
 */
#include "jemalloc/internal/jemalloc_preamble.h"

JEMALLOC_ALWAYS_INLINE unsigned
os_cpu_ncpus(void) {
	long result;

#ifdef CPU_COUNT
	{
		cpu_set_t set;
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

JEMALLOC_ALWAYS_INLINE bool
os_cpu_count_is_deterministic(void) {
	long cpu_onln = sysconf(_SC_NPROCESSORS_ONLN);
	long cpu_conf = sysconf(_SC_NPROCESSORS_CONF);
	if (cpu_onln != cpu_conf) {
		return false;
	}
#	if defined(CPU_COUNT)
	cpu_set_t set;
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
#else
	/*
	 * No sched_getcpu() on macOS; read the CPU number like _os_cpu_number()
	 * does, from the low 12 bits of tpidr_el0 (arm64) or the IDT base (x86).
	 * The 0xfff mask is xnu's __TPIDR_CPU_NUM_MASK, kept in sync with
	 * _os_cpu_number in libsyscall/os/tsd.h (the kernel counterpart is
	 * MACHDEP_TPIDR_CPUNUM_MASK in osfmk/arm64/machine_machdep.h):
	 * https://github.com/apple-oss-distributions/xnu/blob/main/libsyscall/os/tsd.h
	 * This requires macOS 12+: on macOS 11 and earlier arm64 kept the CPU
	 * number in tpidrro_el0's low 3 bits instead, so it would be misread here.
	 * Those releases are retired by Apple, so only the current layout is handled.
	 */
#  if defined(__aarch64__)
	uint64_t cpu;
	__asm__ __volatile__("mrs %0, tpidr_el0" : "=r"(cpu));
	return (int)(cpu & 0xfff);
#  elif defined(__x86_64__) || defined(__i386__)
	struct { uintptr_t p1, p2; } idtr;
	__asm__ __volatile__("sidt %0" : "=m"(idtr));
	return (int)(idtr.p1 & 0xfff);
#  else
	not_reached();
	return -1;
#  endif
#endif
}

JEMALLOC_ALWAYS_INLINE bool
os_cpu_set_affinity(int cpu) {
	(void)cpu;
	return false;
}

#endif /* JEMALLOC_INTERNAL_OS_DARWIN_CPU_H */
