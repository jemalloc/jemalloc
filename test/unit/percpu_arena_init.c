#include "test/jemalloc_test.h"

#if defined(__linux__) && defined(JEMALLOC_HAVE_SCHED_SETAFFINITY)
#	include <sys/wait.h>
#	include "jemalloc/internal/jemalloc_init.h"

const char *malloc_conf_2_conf_harder;
#endif

TEST_BEGIN(test_percpu_restricted_startup) {
#if defined(__linux__) && defined(JEMALLOC_HAVE_SCHED_SETAFFINITY)
	test_skip_if(!have_percpu_arena);

	unsigned cpus[2];
	unsigned n = os_cpu_affinity_cpus(cpus, 2);
	test_skip_if(n == 0 || sysconf(_SC_NPROCESSORS_ONLN) <= 1);

	const char *configs[] = {
	    "narenas:default,background_thread:false,percpu_arena:percpu",
	    "narenas:default,background_thread:false,percpu_arena:phycpu"};
	percpu_arena_mode_t modes[] = {percpu_arena, per_phycpu_arena};

	for (unsigned i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
		pid_t pid = fork();
		assert_d_ge(pid, 0, "Unexpected fork() failure");
		if (pid == 0) {
			/* JET has no constructor; initialize only after pinning. */
			assert_d_eq(malloc_init_state, malloc_init_uninitialized,
			    "Allocator must be uninitialized before pinning");
			if (os_cpu_set_affinity((int)cpus[n - 1])
			    || os_cpu_current() < 0) {
				_exit(test_status_skip);
			}
			/* Reset automatic sizing even if CI supplies narenas. */
			malloc_conf_2_conf_harder = configs[i];

			void *p = mallocx(1024, MALLOCX_TCACHE_NONE);
			assert_ptr_not_null(p, "Unexpected mallocx() failure");
			assert_d_eq(opt_percpu_arena, modes[i],
			    "Requested per-CPU mode must remain enabled");
			assert_u_eq(narenas_auto, 1,
			    "One allowed CPU should require one automatic arena");

			unsigned arena;
			size_t sz = sizeof(arena);
			assert_d_eq(mallctl("arenas.lookup", &arena, &sz, &p,
			    sizeof(p)), 0, "Unexpected arenas.lookup() failure");
			assert_u_eq(arena, 0,
			    "Allocation should use the allowed CPU's arena");
			dallocx(p, MALLOCX_TCACHE_NONE);
			_exit(test_status_pass);
		}

		int status;
		pid_t waited;
		do {
			waited = waitpid(pid, &status, 0);
		} while (waited == -1 && errno == EINTR);
		assert_d_eq(waited, pid, "Unexpected waitpid() failure");
		test_skip_if(WIFEXITED(status)
		    && WEXITSTATUS(status) == test_status_skip);
		expect_true(WIFEXITED(status),
		    "Startup with %s terminated abnormally", configs[i]);
		if (WIFEXITED(status)) {
			expect_d_eq(WEXITSTATUS(status), test_status_pass,
			    "Startup with %s failed", configs[i]);
		}
	}
#else
	test_skip("Requires Linux CPU affinity support");
#endif
}
TEST_END

int
main(void) {
	/* Keep the parent's allocator uninitialized for the child processes. */
	return test_no_malloc_init(test_percpu_restricted_startup);
}
