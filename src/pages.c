#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/base.h"
#include "jemalloc/internal/extent.h"
#include "jemalloc/internal/jemalloc_internal_externs.h"
#include "jemalloc/internal/malloc_io.h"
#include "jemalloc/internal/pages.h"
#include "jemalloc/internal/sc.h"

/******************************************************************************/
/* Data. */

/* Actual operating system page size, detected during bootstrap, <= PAGE. */
size_t os_page;

/* Set here. Consumed by os_vm_reserve/os_vm_commit_impl in os/posix/vm.h. */
int mmap_flags;
/* Set here, consumed directly by the os/overcommit.h and os/vm.h backends. */
bool os_overcommits;

const char *const thp_mode_names[] = {
    "default", "always", "never", "not supported"};
const char *const system_thp_mode_names[] = {
    "madvise", "always", "never", "not supported"};
thp_mode_t        opt_thp = THP_MODE_DEFAULT;
system_thp_mode_t init_system_thp_mode;

/* Runtime support for lazy purge. Irrelevant when !pages_can_purge_lazy. */
static bool pages_can_purge_lazy_runtime = true;

#ifdef JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS
/* Set by os_overcommit_boot()'s madvise_MADV_DONTNEED_zeroes_pages() probe
 * (os/posix/overcommit.h), consumed by pages_purge_forced() below. */
int madvise_dont_need_zeros_is_faulty = -1;
#endif

/******************************************************************************/

static void *
pages_trim(
    void *addr, size_t alloc_size, size_t leadsize, size_t size, bool *commit) {
	return os_vm_trim(addr, alloc_size, leadsize, size, commit);
}

static void *
pages_map_slow(size_t size, size_t alignment, bool *commit) {
	size_t alloc_size = size + alignment - os_page;
	/* Beware size_t wrap-around. */
	if (alloc_size < size) {
		return NULL;
	}

	void *ret;
	do {
		void *pages = os_vm_reserve(NULL, alloc_size, alignment, commit);
		if (pages == NULL) {
			return NULL;
		}
		size_t leadsize = ALIGNMENT_CEILING((uintptr_t)pages, alignment)
		    - (uintptr_t)pages;
		ret = pages_trim(pages, alloc_size, leadsize, size, commit);
	} while (ret == NULL);

	assert(ret != NULL);
	assert(PAGE_ADDR2BASE(ret) == ret);
	return ret;
}

void *
pages_map(void *addr, size_t size, size_t alignment, bool *commit) {
	assert(alignment >= PAGE);
	assert(ALIGNMENT_ADDR2BASE(addr, alignment) == addr);

#ifdef OS_VM_HAS_FIXED_ALIGNED_RESERVE
	return os_vm_reserve_aligned_fixed(addr, size, alignment, commit);
#endif
	/*
	 * Ideally, there would be a way to specify alignment to mmap() (like
	 * NetBSD has), but in the absence of such a feature, we have to work
	 * hard to efficiently create aligned mappings.  The reliable, but
	 * slow method is to create a mapping that is over-sized, then trim the
	 * excess.  However, that always results in one or two calls to
	 * os_vm_release(), and it can leave holes in the process's virtual
	 * memory map if memory grows downward.
	 *
	 * Optimistically try mapping precisely the right amount before falling
	 * back to the slow method, with the expectation that the optimistic
	 * approach works most of the time.
	 */

	void *ret = os_vm_reserve(addr, size, os_page, commit);
	if (ret == NULL || ret == addr) {
		return ret;
	}
	assert(addr == NULL);
	if (ALIGNMENT_ADDR2OFFSET(ret, alignment) != 0) {
		os_vm_release(ret, size);
		return pages_map_slow(size, alignment, commit);
	}

	assert(PAGE_ADDR2BASE(ret) == ret);
	return ret;
}

void
pages_unmap(void *addr, size_t size) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);

	os_vm_release(addr, size);
}

static bool
pages_commit_impl(void *addr, size_t size, bool commit) {
	if (os_overcommits) {
		return true;
	}

	return commit ? os_vm_commit(addr, size) : os_vm_decommit(addr, size);
}

bool
pages_commit(void *addr, size_t size) {
	return pages_commit_impl(addr, size, true);
}

bool
pages_decommit(void *addr, size_t size) {
	return pages_commit_impl(addr, size, false);
}

void
pages_mark_guards(void *head, void *tail) {
	os_vm_mark_guards(head, tail);
}

void
pages_unmark_guards(void *head, void *tail) {
	os_vm_unmark_guards(head, tail);
}

bool
pages_purge_lazy(void *addr, size_t size) {
	assert(ALIGNMENT_ADDR2BASE(addr, os_page) == addr);
	assert(PAGE_CEILING(size) == size);

	if (!pages_can_purge_lazy) {
		return true;
	}
	if (!pages_can_purge_lazy_runtime) {
		/*
		 * Built with lazy purge enabled, but detected it was not
		 * supported on the current system.
		 */
		return true;
	}

	return os_vm_purge_lazy(addr, size);
}

bool
pages_purge_forced(void *addr, size_t size) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);

	if (!pages_can_purge_forced) {
		return true;
	}

#if defined(JEMALLOC_PURGE_MADVISE_DONTNEED)                                   \
    && defined(JEMALLOC_PURGE_MADVISE_DONTNEED_ZEROS)
	return (unlikely(madvise_dont_need_zeros_is_faulty)
	    || madvise(addr, size, MADV_DONTNEED) != 0);
#elif defined(JEMALLOC_PURGE_POSIX_MADVISE_DONTNEED)                           \
    && defined(JEMALLOC_PURGE_POSIX_MADVISE_DONTNEED_ZEROS)
	return (unlikely(madvise_dont_need_zeros_is_faulty)
	    || posix_madvise(addr, size, POSIX_MADV_DONTNEED) != 0);
#elif defined(JEMALLOC_MAPS_COALESCE)
	/* Try to overlay a new demand-zeroed mapping. */
	return pages_commit(addr, size);
#else
	not_reached();
#endif
}

static bool
pages_huge_impl(void *addr, size_t size, bool aligned) {
	if (aligned) {
		assert(HUGEPAGE_ADDR2BASE(addr) == addr);
		assert(HUGEPAGE_CEILING(size) == size);
	}
#if defined(JEMALLOC_HAVE_MADVISE_HUGE)
	return (madvise(addr, size, MADV_HUGEPAGE) != 0);
#elif defined(JEMALLOC_HAVE_MEMCNTL)
	struct memcntl_mha m = {0};
	m.mha_cmd = MHA_MAPSIZE_VA;
	m.mha_pagesize = HUGEPAGE;
	return (memcntl(addr, size, MC_HAT_ADVISE, (caddr_t)&m, 0, 0) == 0);
#else
	return true;
#endif
}

bool
pages_huge(void *addr, size_t size) {
	return pages_huge_impl(addr, size, true);
}

static bool
pages_huge_unaligned(void *addr, size_t size) {
	return pages_huge_impl(addr, size, false);
}

static bool
pages_nohuge_impl(void *addr, size_t size, bool aligned) {
	if (aligned) {
		assert(HUGEPAGE_ADDR2BASE(addr) == addr);
		assert(HUGEPAGE_CEILING(size) == size);
	}

#ifdef JEMALLOC_HAVE_MADVISE_HUGE
	return (madvise(addr, size, MADV_NOHUGEPAGE) != 0);
#else
	return false;
#endif
}

bool
pages_nohuge(void *addr, size_t size) {
	return pages_nohuge_impl(addr, size, true);
}

static bool
pages_nohuge_unaligned(void *addr, size_t size) {
	return pages_nohuge_impl(addr, size, false);
}

bool
pages_collapse(void *addr, size_t size) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);
	/*
	 * There is one more MADV_COLLAPSE precondition that is not easy to
	 * express with assert statement.  In order to madvise(addr, size,
	 * MADV_COLLAPSE) call to be successful, at least one page in the range
	 * must currently be backed by physical memory.  In particularly, this
	 * means we can't call pages_collapse on freshly mapped memory region.
	 * See madvise(2) man page for more details.
	 */
#if defined(JEMALLOC_HAVE_MADVISE_COLLAPSE)                                    \
    && (defined(MADV_COLLAPSE) || defined(JEMALLOC_MADV_COLLAPSE))
#	if defined(MADV_COLLAPSE)
	return (madvise(addr, size, MADV_COLLAPSE) != 0);
#	elif defined(JEMALLOC_MADV_COLLAPSE)
	return (madvise(addr, size, JEMALLOC_MADV_COLLAPSE) != 0);
#	endif
#else
	return true;
#endif
}

bool
pages_dontdump(void *addr, size_t size) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);
#if defined(JEMALLOC_MADVISE_DONTDUMP)
	return madvise(addr, size, MADV_DONTDUMP) != 0;
#elif defined(JEMALLOC_MADVISE_NOCORE)
	return madvise(addr, size, MADV_NOCORE) != 0;
#else
	return false;
#endif
}

bool
pages_dodump(void *addr, size_t size) {
	assert(PAGE_ADDR2BASE(addr) == addr);
	assert(PAGE_CEILING(size) == size);
#if defined(JEMALLOC_MADVISE_DONTDUMP)
	return madvise(addr, size, MADV_DODUMP) != 0;
#elif defined(JEMALLOC_MADVISE_NOCORE)
	return madvise(addr, size, MADV_CORE) != 0;
#else
	return false;
#endif
}

#ifdef JEMALLOC_HAVE_PROCESS_MADVISE
#	include <sys/mman.h>
#	include <sys/syscall.h>

#	ifndef PIDFD_SELF
#		define PIDFD_SELF -10000
#	endif

static atomic_b_t process_madvise_gate = ATOMIC_INIT(true);

static bool
init_process_madvise(void) {
	if (opt_process_madvise_max_batch == 0) {
		return false;
	}

	if (opt_process_madvise_max_batch > PROCESS_MADVISE_MAX_BATCH_LIMIT) {
		opt_process_madvise_max_batch = PROCESS_MADVISE_MAX_BATCH_LIMIT;
	}

	return false;
}

#	ifdef SYS_process_madvise
#		define JE_SYS_PROCESS_MADVISE_NR SYS_process_madvise
#	else
#		define JE_SYS_PROCESS_MADVISE_NR                              \
			EXPERIMENTAL_SYS_PROCESS_MADVISE_NR
#	endif

static bool
pages_purge_process_madvise_impl(
    void *vec, size_t vec_len, size_t total_bytes) {
	if (!atomic_load_b(&process_madvise_gate, ATOMIC_RELAXED)) {
		return true;
	}

	int    saved_errno = get_errno();
	size_t purged_bytes = (size_t)syscall(JE_SYS_PROCESS_MADVISE_NR,
	    PIDFD_SELF, (struct iovec *)vec, vec_len, MADV_DONTNEED, 0);
	if (purged_bytes == (size_t)-1) {
		if (errno == EPERM || errno == EINVAL || errno == ENOSYS
		    || errno == EBADF) {
			/* Process madvise not supported the way we need it. */
			atomic_store_b(
			    &process_madvise_gate, false, ATOMIC_RELAXED);
		}
		set_errno(saved_errno);
	}

	return purged_bytes != total_bytes;
}

#else

static bool
init_process_madvise(void) {
	return false;
}

static bool
pages_purge_process_madvise_impl(
    void *vec, size_t vec_len, size_t total_bytes) {
	not_reached();
	return true;
}

#endif

bool
pages_purge_process_madvise(void *vec, size_t vec_len, size_t total_bytes) {
	return pages_purge_process_madvise_impl(vec, vec_len, total_bytes);
}

static bool
pages_should_skip_set_thp_state(void) {
	if (opt_thp == thp_mode_do_nothing
	    || (opt_thp == thp_mode_always
	        && init_system_thp_mode == system_thp_mode_always)
	    || (opt_thp == thp_mode_never
	        && init_system_thp_mode == system_thp_mode_never)) {
		return true;
	}
	return false;
}
void
pages_set_thp_state(void *ptr, size_t size) {
	if (pages_should_skip_set_thp_state()) {
		return;
	}
	assert(opt_thp != thp_mode_not_supported
	    && init_system_thp_mode != system_thp_mode_not_supported);

	if (opt_thp == thp_mode_always
	    && init_system_thp_mode == system_thp_mode_madvise) {
		pages_huge_unaligned(ptr, size);
	} else if (opt_thp == thp_mode_never) {
		assert(init_system_thp_mode == system_thp_mode_madvise
		    || init_system_thp_mode == system_thp_mode_always);
		pages_nohuge_unaligned(ptr, size);
	}
}

static void
init_thp_state(void) {
	if (!have_madvise_huge && !have_memcntl) {
		if (metadata_thp_enabled() && opt_abort) {
			malloc_write("<jemalloc>: no MADV_HUGEPAGE support\n");
			abort();
		}
		goto label_error;
	}
#if defined(JEMALLOC_HAVE_MADVISE_HUGE)
	static const char sys_state_madvise[] = "always [madvise] never\n";
	static const char sys_state_always[] = "[always] madvise never\n";
	static const char sys_state_never[] = "always madvise [never]\n";
	char              buf[sizeof(sys_state_madvise)];

#	if defined(O_CLOEXEC)
	int fd = malloc_open(
	    "/sys/kernel/mm/transparent_hugepage/enabled", O_RDONLY | O_CLOEXEC);
#	else
	int fd = malloc_open(
	    "/sys/kernel/mm/transparent_hugepage/enabled", O_RDONLY);
	if (fd != -1) {
		fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
	}
#	endif
	if (fd == -1) {
		goto label_error;
	}

	ssize_t nread = malloc_read_fd(fd, &buf, sizeof(buf));
	malloc_close(fd);
	if (nread < 0) {
		goto label_error;
	}

	if (strncmp(buf, sys_state_madvise, (size_t)nread) == 0) {
		init_system_thp_mode = system_thp_mode_madvise;
	} else if (strncmp(buf, sys_state_always, (size_t)nread) == 0) {
		init_system_thp_mode = system_thp_mode_always;
	} else if (strncmp(buf, sys_state_never, (size_t)nread) == 0) {
		init_system_thp_mode = system_thp_mode_never;
	} else {
		goto label_error;
	}
	if (opt_hpa_opts.hugify_style == hpa_hugify_style_auto) {
		if (init_system_thp_mode == system_thp_mode_madvise) {
			opt_hpa_opts.hugify_style = hpa_hugify_style_lazy;
		} else {
			opt_hpa_opts.hugify_style = hpa_hugify_style_none;
		}
	}
	return;
#elif defined(JEMALLOC_HAVE_MEMCNTL)
	init_system_thp_mode = system_thp_mode_madvise;
	if (opt_hpa_opts.hugify_style == hpa_hugify_style_auto) {
		opt_hpa_opts.hugify_style = hpa_hugify_style_eager;
	}
	return;
#endif
label_error:
	opt_thp = thp_mode_not_supported;
	init_system_thp_mode = system_thp_mode_not_supported;
}

bool
pages_boot(void) {
	os_page = os_vm_page_size();
	if (os_page > PAGE) {
		malloc_write("<jemalloc>: Unsupported system page size\n");
		if (opt_abort) {
			abort();
		}
		return true;
	}

	if (os_overcommit_boot()) {
		return true;
	}

	init_thp_state();

#ifdef __FreeBSD__
	/*
	 * FreeBSD doesn't need the check; madvise(2) is known to work.
	 */
#else
	/* Detect lazy purge runtime support. */
	if (pages_can_purge_lazy) {
		bool  committed = false;
		void *madv_free_page = os_vm_reserve(
		    NULL, PAGE, PAGE, &committed);
		if (madv_free_page == NULL) {
			return true;
		}
		assert(pages_can_purge_lazy_runtime);
		if (pages_purge_lazy(madv_free_page, PAGE)) {
			pages_can_purge_lazy_runtime = false;
		}
		os_vm_release(madv_free_page, PAGE);
	}
#endif
	if (init_process_madvise()) {
		if (opt_abort) {
			abort();
		}
		return true;
	}

	return false;
}
