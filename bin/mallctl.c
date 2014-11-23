/*
 * Copyright (C) 2014 Conrad Meyer <cse.cem@gmail.com>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice(s),
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice(s),
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
 * EVENT SHALL THE COPYRIGHT HOLDER(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <err.h>

#include "jemalloc/internal/jemalloc_internal.h"

static void
usage(void)
{

	printf("Usage: mallctl [ctl.name]\n\n"

	    "When invoked with no arguments, prints all defined controls and\n"
	    "their current value.\n\n"

	    "With a control name alone, prints the current value of that control.\n");
	exit(0);
}

static int
mib_next(size_t *mibout, size_t *miblen, size_t *mibin, size_t miblenin)
{
	static size_t next_mib[10], next_miblen = SIZE_MAX;

	int error;

	if (next_miblen == SIZE_MAX) {
		next_miblen = sizeof(next_mib) / sizeof(next_mib[0]);
		error = mallctlnametomib("introspect.next", next_mib, &next_miblen);
		if (error) {
			errno = error;
			err(2, "mallctlnametomib");
		}
	}

	error = mallctlbymib(next_mib, next_miblen,
	    mibout, miblen, mibin, miblenin);
	if (error != 0 && error != ENOENT) {
		errno = error;
		err(2, "mib_next");
	}

	return (error);
}

static const char *
mib_name(size_t *mib, size_t len)
{
	static size_t name_mib[10], name_miblen = SIZE_MAX;
	static char name_buf[256];

	size_t bsz;
	int error;

	if (name_miblen == SIZE_MAX) {
		name_miblen = sizeof(name_mib) / sizeof(name_mib[0]);
		error = mallctlnametomib("introspect.name", name_mib, &name_miblen);
		if (error) {
			errno = error;
			err(2, "mallctlnametomib");
		}
	}

	bsz = sizeof(name_buf);
	error = mallctlbymib(name_mib, name_miblen, name_buf, &bsz, mib, len);
	if (error) {
		errno = error;
		err(2, "mib_name");
	}

	return (name_buf);
}

static unsigned
mib_kind(size_t *mib, size_t len)
{
	static size_t kind_mib[10], kind_miblen = SIZE_MAX;

	size_t usz;
	unsigned res;
	int error;

	if (kind_miblen == SIZE_MAX) {
		kind_miblen = sizeof(kind_mib) / sizeof(kind_mib[0]);
		error = mallctlnametomib("introspect.kind", kind_mib, &kind_miblen);
		if (error) {
			errno = error;
			err(2, "mallctlnametomib");
		}
	}

	usz = sizeof(res);
	error = mallctlbymib(kind_mib, kind_miblen, &res, &usz, mib, len);
	if (error) {
		errno = error;
		err(2, "mib_kind");
	}

	return (res);
}

static size_t type_sizes[MCTLTYPE + 1] = {
	[MCTLTYPE_U64] = sizeof(uint64_t),
	[MCTLTYPE_SIZE] = sizeof(size_t),
	[MCTLTYPE_SSIZE] = sizeof(ssize_t),
	[MCTLTYPE_U32] = sizeof(uint32_t),
	[MCTLTYPE_UINT] = sizeof(unsigned),
	[MCTLTYPE_BOOL] = sizeof(bool),
	[MCTLTYPE_STRP] = sizeof(const char *),
};

static void
fmt_mib(size_t *mib, size_t miblen)
{
	static char buf[512];

	const char *name;
	unsigned kind;
	size_t sz;
	int error;

	kind = mib_kind(mib, miblen * sizeof(*mib));
	name = mib_name(mib, miblen * sizeof(*mib));

	if (kind == MCTLTYPE_NODE) {
		warnx("'%s' isn't a leaf node", name);
		return;
	}

	printf("%s: ", name);

	sz = sizeof(buf);
	if (type_sizes[kind] > 0)
		sz = type_sizes[kind];

	error = mallctlbymib(mib, miblen, buf, &sz, NULL, 0);
	if (error) {
		printf("<error(%d): %s>\n", error, strerror(error));
		return;
	}

	switch (kind) {
	case MCTLTYPE_U64:
		printf("%ju", (uintmax_t) *(uint64_t *)buf);
		break;

	case MCTLTYPE_SIZE:
		printf("%zu", *(size_t *)buf);
		break;

	case MCTLTYPE_SSIZE:
		printf("%zd", *(ssize_t *)buf);
		break;

	case MCTLTYPE_U32:
		printf("%ju", (uintmax_t) *(uint32_t *)buf);
		break;

	case MCTLTYPE_UINT:
		printf("%u", *(unsigned *)buf);
		break;

	case MCTLTYPE_BOOL:
		printf("%u", (unsigned) *(bool *)buf);
		break;

	case MCTLTYPE_STRP:
		printf("%s", *(const char **)buf);
		break;

	case MCTLTYPE_STRING:
		printf("%.*s", (int)sz, buf);
		break;

	case MCTLTYPE_OPAQUE:
		printf("<opaque>");
		break;

	default:
		printf("<unrecognized kind %u>", kind);
		break;
	}

	printf("\n");
}

int
main(int argc, char **argv)
{
	size_t mib[10], miblen;
	int error;

	if (argc > 2)
		usage();

	/* XXX: One could also list all ctl's below a specific node. */
	if (argc > 1) {
		if (strcmp(argv[1], "-h") == 0 ||
		    strcmp(argv[1], "--help") == 0)
			usage();

		miblen = sizeof(mib) / sizeof(mib[0]);
		error = mallctlnametomib(argv[1], mib, &miblen);
		if (error == ENOENT) {
			printf("No such ctl `%s'\n", argv[1]);
			return (1);
		} else if (error) {
			errno = error;
			err(2, "mallctlnametomib");
		}

		fmt_mib(mib, miblen);
	} else {
		size_t lastlen = 0;

		while (true) {
			miblen = sizeof(mib);
			error = mib_next(mib, &miblen, mib, lastlen);
			if (error == ENOENT)
				break;

			lastlen = miblen;
			fmt_mib(mib, miblen / sizeof(mib[0]));
		}
	}

	return (0);
}
