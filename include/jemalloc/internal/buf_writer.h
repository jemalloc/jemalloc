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
	size_t buf_size; /* must be one less than the capacity of buf array */
	size_t buf_end;
} buf_write_arg_t;

void buf_write_flush(buf_write_arg_t *arg);
void buf_write_cb(void *buf_write_arg, const char *s);

#endif /* JEMALLOC_INTERNAL_BUF_WRITER_H */
