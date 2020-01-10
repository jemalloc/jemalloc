#define JEMALLOC_BUF_WRITER_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/buf_writer.h"
#include "jemalloc/internal/malloc_io.h"

void
buf_write_flush(buf_write_arg_t *arg) {
	assert(arg->buf_end <= arg->buf_size);
	arg->buf[arg->buf_end] = '\0';
	if (arg->write_cb == NULL) {
		arg->write_cb = je_malloc_message != NULL ?
		    je_malloc_message : wrtmessage;
	}
	arg->write_cb(arg->cbopaque, arg->buf);
	arg->buf_end = 0;
}

void
buf_write_cb(void *buf_write_arg, const char *s) {
	buf_write_arg_t *arg = (buf_write_arg_t *)buf_write_arg;
	size_t i, slen, n, s_remain, buf_remain;
	assert(arg->buf_end <= arg->buf_size);
	for (i = 0, slen = strlen(s); i < slen; i += n) {
		if (arg->buf_end == arg->buf_size) {
			buf_write_flush(arg);
		}
		s_remain = slen - i;
		buf_remain = arg->buf_size - arg->buf_end;
		n = s_remain < buf_remain ? s_remain : buf_remain;
		memcpy(arg->buf + arg->buf_end, s + i, n);
		arg->buf_end += n;
	}
	assert(i == slen);
}
