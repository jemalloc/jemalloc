#define	JEMALLOC_TRACE_C_
#include "internal/jemalloc_internal.h"
#ifdef JEMALLOC_TRACE
/******************************************************************************/
/* Data. */

bool				opt_trace = false;

static malloc_mutex_t		trace_mtx;
static unsigned			trace_next_tid = 1;

static unsigned __thread	trace_tid
    JEMALLOC_ATTR(tls_model("initial-exec"));
/* Used to cause trace_cleanup() to be called. */
static pthread_key_t		trace_tsd;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static arena_t	*trace_arena(const void *ptr);
static void	trace_flush(arena_t *arena);
static void	trace_write(arena_t *arena, const char *s);
static unsigned	trace_get_tid(void);
static void	trace_thread_cleanup(void *arg);
static void	malloc_trace_flush_all(void);

/******************************************************************************/

static arena_t *
trace_arena(const void *ptr)
{
	arena_t *arena;
	arena_chunk_t *chunk;

	chunk = (arena_chunk_t *)CHUNK_ADDR2BASE(ptr);
	if ((void *)chunk == ptr)
		arena = arenas[0];
	else
		arena = chunk->arena;

	return (arena);
}

static void
trace_flush(arena_t *arena)
{
	ssize_t err;

	err = write(arena->trace_fd, arena->trace_buf, arena->trace_buf_end);
	if (err == -1) {
		malloc_write4("<jemalloc>",
		    ": write() failed during trace flush", "\n", "");
		abort();
	}
	arena->trace_buf_end = 0;
}

static void
trace_write(arena_t *arena, const char *s)
{
	unsigned i, slen, n;

	i = 0;
	slen = strlen(s);
	while (i < slen) {
		/* Flush the trace buffer if it is full. */
		if (arena->trace_buf_end == TRACE_BUF_SIZE)
			trace_flush(arena);

		if (arena->trace_buf_end + slen <= TRACE_BUF_SIZE) {
			/* Finish writing. */
			n = slen - i;
		} else {
			/* Write as much of s as will fit. */
			n = TRACE_BUF_SIZE - arena->trace_buf_end;
		}
		memcpy(&arena->trace_buf[arena->trace_buf_end], &s[i], n);
		arena->trace_buf_end += n;
		i += n;
	}
}

static unsigned
trace_get_tid(void)
{
	unsigned ret = trace_tid;

	if (ret == 0) {
		malloc_mutex_lock(&trace_mtx);
		trace_tid = trace_next_tid;
		trace_next_tid++;
		malloc_mutex_unlock(&trace_mtx);
		ret = trace_tid;

		/*
		 * Set trace_tsd to non-zero so that the cleanup function will
		 * be called upon thread exit.
		 */
		pthread_setspecific(trace_tsd, (void *)ret);
	}

	return (ret);
}

static void
malloc_trace_flush_all(void)
{
	unsigned i;

	for (i = 0; i < narenas; i++) {
		if (arenas[i] != NULL) {
			malloc_mutex_lock(&arenas[i]->lock);
			trace_flush(arenas[i]);
			malloc_mutex_unlock(&arenas[i]->lock);
		}
	}
}

void
trace_malloc(const void *ptr, size_t size)
{
	char buf[UMAX2S_BUFSIZE];
	arena_t *arena = trace_arena(ptr);

	malloc_mutex_lock(&arena->lock);

	trace_write(arena, umax2s(trace_get_tid(), 10, buf));
	trace_write(arena, " m 0x");
	trace_write(arena, umax2s((uintptr_t)ptr, 16, buf));
	trace_write(arena, " ");
	trace_write(arena, umax2s(size, 10, buf));
	trace_write(arena, "\n");

	malloc_mutex_unlock(&arena->lock);
}

void
trace_calloc(const void *ptr, size_t number, size_t size)
{
	char buf[UMAX2S_BUFSIZE];
	arena_t *arena = trace_arena(ptr);

	malloc_mutex_lock(&arena->lock);

	trace_write(arena, umax2s(trace_get_tid(), 10, buf));
	trace_write(arena, " c 0x");
	trace_write(arena, umax2s((uintptr_t)ptr, 16, buf));
	trace_write(arena, " ");
	trace_write(arena, umax2s(number, 10, buf));
	trace_write(arena, " ");
	trace_write(arena, umax2s(size, 10, buf));
	trace_write(arena, "\n");

	malloc_mutex_unlock(&arena->lock);
}

void
trace_posix_memalign(const void *ptr, size_t alignment, size_t size)
{
	char buf[UMAX2S_BUFSIZE];
	arena_t *arena = trace_arena(ptr);

	malloc_mutex_lock(&arena->lock);

	trace_write(arena, umax2s(trace_get_tid(), 10, buf));
	trace_write(arena, " a 0x");
	trace_write(arena, umax2s((uintptr_t)ptr, 16, buf));
	trace_write(arena, " ");
	trace_write(arena, umax2s(alignment, 10, buf));
	trace_write(arena, " ");
	trace_write(arena, umax2s(size, 10, buf));
	trace_write(arena, "\n");

	malloc_mutex_unlock(&arena->lock);
}

void
trace_realloc(const void *ptr, const void *old_ptr, size_t size,
    size_t old_size)
{
	char buf[UMAX2S_BUFSIZE];
	arena_t *arena = trace_arena(ptr);

	malloc_mutex_lock(&arena->lock);

	trace_write(arena, umax2s(trace_get_tid(), 10, buf));
	trace_write(arena, " r 0x");
	trace_write(arena, umax2s((uintptr_t)ptr, 16, buf));
	trace_write(arena, " 0x");
	trace_write(arena, umax2s((uintptr_t)old_ptr, 16, buf));
	trace_write(arena, " ");
	trace_write(arena, umax2s(size, 10, buf));
	trace_write(arena, " ");
	trace_write(arena, umax2s(old_size, 10, buf));
	trace_write(arena, "\n");

	malloc_mutex_unlock(&arena->lock);
}

void
trace_free(const void *ptr, size_t size)
{
	char buf[UMAX2S_BUFSIZE];
	arena_t *arena = trace_arena(ptr);

	malloc_mutex_lock(&arena->lock);

	trace_write(arena, umax2s(trace_get_tid(), 10, buf));
	trace_write(arena, " f 0x");
	trace_write(arena, umax2s((uintptr_t)ptr, 16, buf));
	trace_write(arena, " ");
	trace_write(arena, umax2s(isalloc(ptr), 10, buf));
	trace_write(arena, "\n");

	malloc_mutex_unlock(&arena->lock);
}

void
trace_malloc_usable_size(size_t size, const void *ptr)
{
	char buf[UMAX2S_BUFSIZE];
	arena_t *arena = trace_arena(ptr);

	malloc_mutex_lock(&arena->lock);

	trace_write(arena, umax2s(trace_get_tid(), 10, buf));
	trace_write(arena, " s ");
	trace_write(arena, umax2s(size, 10, buf));
	trace_write(arena, " 0x");
	trace_write(arena, umax2s((uintptr_t)ptr, 16, buf));
	trace_write(arena, "\n");

	malloc_mutex_unlock(&arena->lock);
}

void
trace_thread_exit(void)
{
	char buf[UMAX2S_BUFSIZE];
	arena_t *arena = choose_arena();

	malloc_mutex_lock(&arena->lock);

	trace_write(arena, umax2s(trace_get_tid(), 10, buf));
	trace_write(arena, " x\n");

	malloc_mutex_unlock(&arena->lock);
}

static void
trace_thread_cleanup(void *arg)
{

	trace_thread_exit();
}

bool
trace_boot(void)
{

	if (malloc_mutex_init(&trace_mtx))
		return (true);

	/* Flush trace buffers at exit. */
	atexit(malloc_trace_flush_all);
	/* Receive thread exit notifications. */
	if (pthread_key_create(&trace_tsd, trace_thread_cleanup) != 0) {
		malloc_write4("<jemalloc>",
		    ": Error in pthread_key_create()\n", "", "");
		abort();
	}

	return (false);
}
/******************************************************************************/
#endif /* JEMALLOC_TRACE */
