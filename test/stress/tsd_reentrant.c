#include "test/jemalloc_test.h"

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
#endif

#ifndef _WIN32

extern void (*je_test_hooks_tsd_bootstrap_hook)(void);

static atomic_zu_t handler_allocs;

static atomic_b_t crash_reported;
static bool hook_ran;
static volatile sig_atomic_t in_reentrant_signal_alloc;

static void *volatile signal_alloc;

static void
on_crash(int signo) {
#define WRITE_LITERAL(s) (void)write(STDERR_FILENO, s, sizeof(s) - 1)
	if (atomic_exchange_b(&crash_reported, true, ATOMIC_RELAXED)) {
		_exit(128 + signo);
	}

	WRITE_LITERAL("\n*** tsd_reentrant reproduced allocator crash ***\n");
	if (signo == SIGSEGV) {
		WRITE_LITERAL("signal: SIGSEGV\n");
	} else if (signo == SIGBUS) {
		WRITE_LITERAL("signal: SIGBUS\n");
	}
	WRITE_LITERAL("inside SIGUSR1 handler malloc: ");
	if (in_reentrant_signal_alloc) {
		WRITE_LITERAL("yes\n");
	} else {
		WRITE_LITERAL("no\n");
	}
	WRITE_LITERAL("This matches recursive allocation during TSD/tcache "
	    "bootstrap.\n");
#undef WRITE_LITERAL

	_exit(128 + signo);
}

static void
on_signal(int signo) {
	(void)signo;
	in_reentrant_signal_alloc = 1;

	/*
	 * This intentionally models asynchronous interruption that allocates.
	 * jemalloc does not promise that arbitrary signal-handler allocation is
	 * safe, but the allocator should keep its own bootstrap state
	 * internally consistent if such reentrancy occurs.
	 */
	void *p = malloc(16);
	if (p != NULL) {
		signal_alloc = p;
		atomic_load_add_store_zu(&handler_allocs, 1);
	}
	in_reentrant_signal_alloc = 0;
}

static void
tsd_bootstrap_signal_hook(void) {
	hook_ran = true;
	je_test_hooks_tsd_bootstrap_hook = NULL;
	/*
	 * The hook only selects the bootstrap window.  The recursive allocation
	 * still enters through the process signal path and public malloc().
	 */
	raise(SIGUSR1);
}

static void *
worker_start(void *arg) {
	(void)arg;
	void *p = malloc(16);
	expect_ptr_not_null(p, "Unexpected malloc() failure");
	if (p != NULL) {
		memset(p, 0xa5, 16);
		free(p);
	}

	je_test_hooks_tsd_bootstrap_hook = NULL;

	return NULL;
}

static void
init_signal_handler(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	expect_d_eq(sigaction(SIGUSR1, &sa, NULL), 0,
	    "Unexpected sigaction() failure");

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_crash;
	sigemptyset(&sa.sa_mask);
	expect_d_eq(sigaction(SIGSEGV, &sa, NULL), 0,
	    "Unexpected sigaction() failure");
	expect_d_eq(sigaction(SIGBUS, &sa, NULL), 0,
	    "Unexpected sigaction() failure");
}

TEST_BEGIN(test_tsd_bootstrap_signal_reentrant) {
	thd_t worker;

	init_signal_handler();

	atomic_store_b(&crash_reported, false, ATOMIC_RELEASE);
	atomic_store_zu(&handler_allocs, 0, ATOMIC_RELEASE);
	hook_ran = false;

	je_test_hooks_tsd_bootstrap_hook = tsd_bootstrap_signal_hook;
	thd_create(&worker, worker_start, NULL);
	thd_join(worker, NULL);
	je_test_hooks_tsd_bootstrap_hook = NULL;

	expect_true(hook_ran, "TSD bootstrap hook should have executed");
	expect_zu_eq(atomic_load_zu(&handler_allocs, ATOMIC_ACQUIRE), 1,
	    "Signal handler should have allocated");
}
TEST_END

#endif

int
main(void) {
#ifdef _WIN32
	test_skip("Signals are unavailable");
	return test_status_skip;
#else
	return test_no_reentrancy(test_tsd_bootstrap_signal_reentrant);
#endif
}
