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

typedef void (write_cb_t)(void *, const char *);

typedef struct {
	write_cb_t *public_write_cb;
	void *public_cbopaque;
	write_cb_t *private_write_cb;
	void *private_cbopaque;
	char *buf;
	size_t buf_size;
	size_t buf_end;
	bool internal_buf;
} buf_writer_t;

bool buf_writer_init(tsdn_t *tsdn, buf_writer_t *buf_writer,
    write_cb_t *write_cb, void *cbopaque, char *buf, size_t buf_len);
write_cb_t *buf_writer_get_write_cb(buf_writer_t *buf_writer);
void *buf_writer_get_cbopaque(buf_writer_t *buf_writer);
void buf_writer_flush(buf_writer_t *buf_writer);
void buf_writer_terminate(tsdn_t *tsdn, buf_writer_t *buf_writer);

#endif /* JEMALLOC_INTERNAL_BUF_WRITER_H */
