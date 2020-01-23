#define JEMALLOC_BUF_WRITER_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/buf_writer.h"
#include "jemalloc/internal/malloc_io.h"

void
buf_writer_flush(buf_writer_t *buf_writer) {
	assert(buf_writer->buf_end <= buf_writer->buf_size);
	buf_writer->buf[buf_writer->buf_end] = '\0';
	if (buf_writer->write_cb == NULL) {
		buf_writer->write_cb = je_malloc_message != NULL ?
		    je_malloc_message : wrtmessage;
	}
	buf_writer->write_cb(buf_writer->cbopaque, buf_writer->buf);
	buf_writer->buf_end = 0;
}

void
buf_writer_cb(void *buf_writer_arg, const char *s) {
	buf_writer_t *buf_writer = (buf_writer_t *)buf_writer_arg;
	size_t i, slen, n, s_remain, buf_remain;
	assert(buf_writer->buf_end <= buf_writer->buf_size);
	for (i = 0, slen = strlen(s); i < slen; i += n) {
		if (buf_writer->buf_end == buf_writer->buf_size) {
			buf_writer_flush(buf_writer);
		}
		s_remain = slen - i;
		buf_remain = buf_writer->buf_size - buf_writer->buf_end;
		n = s_remain < buf_remain ? s_remain : buf_remain;
		memcpy(buf_writer->buf + buf_writer->buf_end, s + i, n);
		buf_writer->buf_end += n;
	}
	assert(i == slen);
}
