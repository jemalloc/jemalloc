/*-
 * Copyright (C) 2009 Facebook, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * * Neither the name of Facebook, Inc. nor the names of its contributors may
 *   be used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *******************************************************************************
 *
 * Copyright (C) 2006-2007 Jason Evans <jasone@FreeBSD.org>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice(s), this list of conditions and the following disclaimer as
 *    the first lines of this file unmodified other than the possible
 *    addition of one or more copyright notices.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice(s), this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *******************************************************************************
 */

// Parse a malloc trace and simulate the allocation events.  Records look like:
//
//  <jemalloc>:utrace: 31532 malloc_init()
//  <jemalloc>:utrace: 31532 0x800b01000 = malloc(34816)
//  <jemalloc>:utrace: 31532 free(0x800b0a400)
//  <jemalloc>:utrace: 31532 0x800b35230 = realloc(0x800b35230, 45)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/types.h>

#include "jemalloc_defs.h"
#ifndef JEMALLOC_DEBUG
#  define NDEBUG
#endif
#include <assert.h>
#include "rb.h"

typedef struct record_s record_t;
struct record_s {
	rb_node(record_t)	link;
	uintptr_t		tag;
	void			*obj;
};
typedef rb_tree(record_t) record_tree_t;

static int
record_comp(record_t *a, record_t *b)
{

	if (a->tag < b->tag)
		return (-1);
	else if (a->tag == b->tag)
		return (0);
	else
		return (1);
}

rb_wrap(static JEMALLOC_UNUSED, record_tree_, record_tree_t, record_t, link,
    record_comp)

static record_t *rec_stack = NULL;

static record_t *
record_alloc(void)
{
	record_t *ret;

	if (rec_stack == NULL) {
		record_t *recs;
		unsigned long i;
#define MMAP_SIZE (1024 * 1024 * 1024)

		recs = (record_t *)mmap(NULL, MMAP_SIZE,
		    PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (recs == NULL) {
			fprintf(stderr, "mtrplay: mmap() error (OOM)\n");
			exit(1);
		}

		for (i = 0; i < MMAP_SIZE / sizeof(record_t); i++) {
			recs[i].obj = (void *)rec_stack;
			rec_stack = &recs[i];
		}
	}

	ret = rec_stack;
	rec_stack = (record_t *)ret->obj;

	return (ret);
}

static void
record_dalloc(record_t *rec)
{

	rec->obj = (void *)rec_stack;
	rec_stack = rec;
}

int
main(void)
{
	uint64_t line;
	int result;
	char fbuf[4096], lbuf[128];
	unsigned foff, flen, i;
	unsigned pid;
	void *addr, *oldaddr;
	size_t size;
	record_tree_t tree;

	record_tree_new(&tree);

	foff = flen = 0;
	for (line = 1;; line++) {
		if (line % 100000 == 0)
			fprintf(stderr, ".");

		// Get a line of input.
		for (i = 0; i < sizeof(lbuf) - 1; i++) {
			// Refill the read buffer if necessary.  We use read(2)
			// instead of a higher level API in order to avoid
			// internal allocation.
			if (foff == flen) {
				foff = 0;
				flen = read(0, fbuf, sizeof(fbuf));
				if (flen <= 0) {
					goto RETURN;
				}
			}
			switch (fbuf[foff]) {
				case '\n': {
					lbuf[i] = '\0';
					foff++;
					goto OUT;
				} default: {
					lbuf[i] = fbuf[foff];
					foff++;
					break;
				}
			}
		}
		OUT:;

		// realloc?
		result = sscanf(lbuf,
				"<jemalloc>:utrace: %u %p = realloc(%p, %zu)",
				&pid, &addr, &oldaddr, &size);
		if (result == 4) {
			record_t key, *rec;
			void *p;

			key.tag = (uintptr_t)oldaddr;
			rec = record_tree_search(&tree, &key);
			if (rec == NULL) {
				fprintf(stderr,
				    "mtrplay: Line %"PRIu64
				    ": Record not found\n", line);
				exit(1);
			}
			record_tree_remove(&tree, rec);

			p = realloc(rec->obj, size);
			if (p == NULL) {
				fprintf(stderr, "mtrplay: Line %"PRIu64
				    ": OOM\n", line);
				exit(1);
			}

			rec->tag = (uintptr_t)addr;
			rec->obj = p;
			record_tree_insert(&tree, rec);
			continue;
		}

		// malloc?
		result = sscanf(lbuf, "<jemalloc>:utrace: %u %p = malloc(%zu)",
				&pid, &addr, &size);
		if (result == 3)
		{
			void *p;
			record_t *rec;

			rec = record_alloc();

			p = malloc(size);
			if (p == NULL) {
				fprintf(stderr, "mtrplay: Line %"PRIu64
				    ": OOM\n", line);
				exit(1);
			}
			memset(p, 0, size);

			rec->tag = (uintptr_t)addr;
			rec->obj = p;
			record_tree_insert(&tree, rec);
			continue;
		}

		// free?
		result = sscanf(lbuf, "<jemalloc>:utrace: %u free(%p)",
				&pid, &oldaddr);
		if (result == 2)
		{
			record_t key, *rec;

			key.tag = (uintptr_t)oldaddr;
			rec = record_tree_search(&tree, &key);
			if (rec == NULL) {
				fprintf(stderr,
				    "mtrplay: Line %"PRIu64
				    ": Record not found\n", line);
				exit(1);
			}
			record_tree_remove(&tree, rec);

			free(rec->obj);
			record_dalloc(rec);
			continue;
		}

		// malloc_init?
		result = sscanf(lbuf, "<jemalloc>:utrace: %u malloc_init()",
				&pid);
		if (result == 1)
		{
			continue;
		}

		fprintf(stderr, "mtrplay: Error reading line %"PRIu64
		    " of input\n", line);
		exit(1);
	}

RETURN:
	return 0;
}
