#ifndef JEMALLOC_INTERNAL_DEFERRAL_H
#define JEMALLOC_INTERNAL_DEFERRAL_H

#include "jemalloc/internal/jemalloc_preamble.h"

/*
 * The deferred-work contract shared by the page allocators that PRODUCE
 * deferred work (PAC decay-purge, HPA purge/hugify) and the drivers that
 * CONSUME it (the background thread, or the application-inline fallback).
 *
 * There is deliberately no polymorphic interface here: with only two
 * page allocators, a vtable's indirect calls are needless and inefficient.
 * Dispatch is direct pac_*()/hpa_*(), so the contract is a convention
 * documented here, not an enforced type.  The only shared code it needs is the
 * constants below; each allocator keeps its own policy definition (functions
 * and constants) in its own header.
 *
 * The convention every page allocator follows:
 *   - <alloc>_time_until_deferred_work(): nanoseconds until its next
 *     deferred work is due.  DEFERRED_WORK_MIN = "due now",
 *     DEFERRED_WORK_MAX = "nothing pending"; anything between is a real
 *     ns deadline.
 *   - <alloc>_do_deferred_work(): perform whatever work is due now.
 * The pa layer reports the soonest deadline across its allocators; the
 * background thread sleeps until then, or indefinitely on DEFERRED_WORK_MAX.
 */
#define DEFERRED_WORK_MIN UINT64_C(0)
#define DEFERRED_WORK_MAX UINT64_MAX

#endif /* JEMALLOC_INTERNAL_DEFERRAL_H */
