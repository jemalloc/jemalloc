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
	    "abort_conf:true,narenas:default,background_thread:false,"
	    "percpu_arena:percpu",
	    "abort_conf:true,narenas:default,background_thread:false,"
	    "percpu_arena:phycpu"};

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
			assert_d_eq(opt_percpu_arena, percpu_arena,
			    "Both spellings must enable per-CPU arenas");
			assert_u_eq(narenas_auto, 1,
			    "One allowed CPU should require one automatic arena");
			const char *mode;
			size_t sz = sizeof(mode);
			assert_d_eq(mallctl("opt.percpu_arena", &mode, &sz,
			    NULL, 0), 0, "Unexpected opt.percpu_arena failure");
			assert_str_eq(mode, "percpu",
			    "Both spellings must report the canonical mode");

			unsigned arena;
			sz = sizeof(arena);
			assert_d_eq(mallctl("arenas.lookup", &arena, &sz, &p,
			    sizeof(p)), 0, "Unexpected arenas.lookup() failure");
			assert_u_eq(arena, 0,
			    "Allocation should use the allowed CPU's arena");
			dallocx(p, MALLOCX_TCACHE_NONE);

			if (n > 1) {
				/* Move to a CPU excluded from the startup mask. */
				assert_false(os_cpu_set_affinity((int)cpus[0]),
				    "Could not change affinity after startup");
				assert_d_eq(os_cpu_current(), (int)cpus[0],
				    "Thread must run on the newly allowed CPU");
				p = mallocx(1024, MALLOCX_TCACHE_NONE);
				assert_ptr_not_null(p,
				    "Allocation after affinity change must succeed");
				sz = sizeof(arena);
				assert_d_eq(mallctl("arenas.lookup", &arena, &sz,
				    &p, sizeof(p)), 0,
				    "Unexpected arenas.lookup() failure");
				assert_u_eq(arena, 0,
				    "New CPU must share the original arena range");
				assert_u_eq(narenas_auto, 1,
				    "Affinity changes must not resize the range");
				dallocx(p, MALLOCX_TCACHE_NONE);
			}
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
