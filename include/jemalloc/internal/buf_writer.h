#ifndef JEMALLOC_INTERNAL_BUF_WRITER_H
#define JEMALLOC_INTERNAL_BUF_WRITER_H

/*
 * Note: when using the buffered writer, cbopaque is passed to write_cb only
 * when the buffer is flushed.  It would make a difference if cbopaque points
 * to something that's changing for each write_cb call, or something that
 * affects write_cb in a way dependent on the content of the output string.
 * However, the most typical usage case in practice is that cbopaque points to
 * some "option like" content for the write_cb, so it doesn't matter.
 */

typedef struct {
	void (*write_cb)(void *, const char *);
	void *cbopaque;
	char *buf;
	size_t buf_size;
	size_t buf_end;
} buf_write_arg_t;

JEMALLOC_ALWAYS_INLINE void
buf_write_init(buf_write_arg_t *arg, void (*write_cb)(void *, const char *),
    void *cbopaque, char *buf, size_t buf_len) {
	arg->write_cb = write_cb;
	arg->cbopaque = cbopaque;
	arg->buf = buf;
	arg->buf_size = buf_len - 1; /* Accommodating '\0' at the end. */
	arg->buf_end = 0;
}

void buf_write_flush(buf_write_arg_t *arg);
void buf_write_cb(void *buf_write_arg, const char *s);

#endif /* JEMALLOC_INTERNAL_BUF_WRITER_H */
