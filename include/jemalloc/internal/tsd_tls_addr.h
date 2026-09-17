#ifndef JEMALLOC_INTERNAL_TSD_TLS_ADDR_H
#define JEMALLOC_INTERNAL_TSD_TLS_ADDR_H

#include "jemalloc/internal/jemalloc_preamble.h"

/**
 * Under --enable-experimental-fiber-safe-tls generates accessors for TLS variables
 * that does not allow compiler to hoist it.
 *
 * A raw `&tlsvar` is `thread_pointer + const_offset`, inlined into malloc/free under LTO
 * can be hoisted across a swapcontext and reused on the OS thread a fiber migrated to -
 * a stale tsd/tcache and silent heap corruption.
 */
#ifdef JEMALLOC_EXPERIMENTAL_FIBER_SAFE_TLS
#  ifndef __GNUC__
#    error "--enable-experimental-fiber-safe-tls requires GNU-style inline asm"
#  endif
#  define JEMALLOC_TLS_ADDR_DECLARE(tlsvar)                                    \
	__typeof__(&(tlsvar)) jemalloc_tls_addr_##tlsvar(void);
#  define JEMALLOC_TLS_ADDR_DEFINE(tlsvar)                                     \
	JEMALLOC_NOINLINE __typeof__(&(tlsvar))                                       \
	jemalloc_tls_addr_##tlsvar(void) {                                            \
		__typeof__(&(tlsvar)) tls_addr = &(tlsvar);                                  \
		__asm__ __volatile__("" : "+r"(tls_addr) : : "memory");                      \
		return tls_addr;                                                             \
	}
#  define JEMALLOC_TLS_ADDR(tlsvar) (jemalloc_tls_addr_##tlsvar())
#else
#  define JEMALLOC_TLS_ADDR_DECLARE(tlsvar)
#  define JEMALLOC_TLS_ADDR_DEFINE(tlsvar)
#  define JEMALLOC_TLS_ADDR(tlsvar) (&(tlsvar))
#endif

#endif /* JEMALLOC_INTERNAL_TSD_TLS_ADDR_H */
