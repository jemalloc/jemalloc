#define JEMALLOC_BUF_WRITER_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/buf_writer.h"
#include "jemalloc/internal/malloc_io.h"

static void *
buf_writer_allocate_internal_buf(tsdn_t *tsdn, size_t buf_len) {
#ifdef JEMALLOC_JET
	if (buf_len > SC_LARGE_MAXCLASS) {
		return NULL;
	}
#else
	assert(buf_len <= SC_LARGE_MAXCLASS);
#endif
	return iallocztm(tsdn, buf_len, sz_size2index(buf_len), false, NULL,
	    true, arena_get(tsdn, 0, false), true);
}

static void
buf_writer_free_internal_buf(tsdn_t *tsdn, void *buf) {
	if (buf != NULL) {
		idalloctm(tsdn, buf, NULL, NULL, true, true);
	}
}

static write_cb_t buf_writer_cb;

static void
buf_writer_assert(buf_writer_t *buf_writer) {
	if (buf_writer->buf != NULL) {
		assert(buf_writer->public_write_cb == buf_writer_cb);
		assert(buf_writer->public_cbopaque == buf_writer);
		assert(buf_writer->private_write_cb != buf_writer_cb);
		assert(buf_writer->private_cbopaque != buf_writer);
		assert(buf_writer->buf_size > 0);
	} else {
		assert(buf_writer->public_write_cb != buf_writer_cb);
		assert(buf_writer->public_cbopaque != buf_writer);
		assert(buf_writer->private_write_cb == NULL);
		assert(buf_writer->private_cbopaque == NULL);
		assert(buf_writer->buf_size == 0);
	}
}

bool
buf_writer_init(tsdn_t *tsdn, buf_writer_t *buf_writer, write_cb_t *write_cb,
    void *cbopaque, char *buf, size_t buf_len) {
	assert(buf_len >= 2);
	if (buf != NULL) {
		buf_writer->buf = buf;
		buf_writer->internal_buf = false;
	} else {
		buf_writer->buf = buf_writer_allocate_internal_buf(tsdn,
		    buf_len);
		buf_writer->internal_buf = true;
	}
	buf_writer->buf_end = 0;
	if (buf_writer->buf != NULL) {
		buf_writer->public_write_cb = buf_writer_cb;
		buf_writer->public_cbopaque = buf_writer;
		buf_writer->private_write_cb = write_cb;
		buf_writer->private_cbopaque = cbopaque;
		buf_writer->buf_size = buf_len - 1; /* Allowing for '\0'. */
		buf_writer_assert(buf_writer);
		return false;
	} else {
		buf_writer->public_write_cb = write_cb;
		buf_writer->public_cbopaque = cbopaque;
		buf_writer->private_write_cb = NULL;
		buf_writer->private_cbopaque = NULL;
		buf_writer->buf_size = 0;
		buf_writer_assert(buf_writer);
		return true;
	}
}

write_cb_t *
buf_writer_get_write_cb(buf_writer_t *buf_writer) {
	buf_writer_assert(buf_writer);
	return buf_writer->public_write_cb;
}

void *
buf_writer_get_cbopaque(buf_writer_t *buf_writer) {
	buf_writer_assert(buf_writer);
	return buf_writer->public_cbopaque;
}

void
buf_writer_flush(buf_writer_t *buf_writer) {
	buf_writer_assert(buf_writer);
	if (buf_writer->buf == NULL) {
		return;
	}
	assert(buf_writer->buf_end <= buf_writer->buf_size);
	buf_writer->buf[buf_writer->buf_end] = '\0';
	if (buf_writer->private_write_cb == NULL) {
		buf_writer->private_write_cb = je_malloc_message != NULL ?
		    je_malloc_message : wrtmessage;
	}
	assert(buf_writer->private_write_cb != NULL);
	buf_writer->private_write_cb(buf_writer->private_cbopaque,
	    buf_writer->buf);
	buf_writer->buf_end = 0;
}

static void
buf_writer_cb(void *buf_writer_arg, const char *s) {
	buf_writer_t *buf_writer = (buf_writer_t *)buf_writer_arg;
	buf_writer_assert(buf_writer);
	assert(buf_writer->buf != NULL);
	assert(buf_writer->buf_end <= buf_writer->buf_size);
	size_t i, slen, n, s_remain, buf_remain;
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

void
buf_writer_terminate(tsdn_t *tsdn, buf_writer_t *buf_writer) {
	buf_writer_assert(buf_writer);
	buf_writer_flush(buf_writer);
	if (buf_writer->internal_buf) {
		buf_writer_free_internal_buf(tsdn, buf_writer->buf);
	}
}
