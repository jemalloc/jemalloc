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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <errno.h>
#include <string.h>

#include <gd.h>
#include <gdfontt.h>
#include <gdfonts.h>
#include <gdfontmb.h>
#include <gdfontl.h>
#include <gdfontg.h>

#include "jemalloc.h"
#ifndef JEMALLOC_DEBUG
#  define NDEBUG
#endif
#include <assert.h>
#include "rb.h"

typedef struct
{
    // malloc: {0, size, addr}
    // realloc: {oldAddr, size, addr}
    // free: {oldAddr, 0, 0}
    uintptr_t oldAddr;
    size_t size;
    uintptr_t addr;
} TraceRecord;

typedef struct
{
    TraceRecord *trace;
    uint64_t traceLen;
} Trace;

typedef struct AllocationCacheRecordStruct AllocationCacheRecord;
struct AllocationCacheRecordStruct
{
    rb_node(AllocationCacheRecord) link;

    uint64_t addr;
    uint64_t size;
};
typedef rb_tree(AllocationCacheRecord) AllocationCache;

int
cacheComp(AllocationCacheRecord *a, AllocationCacheRecord *b)
{
    if (a->addr < b->addr)
    {
	return -1;
    }
    else if (a->addr > b->addr)
    {
	return 1;
    }
    else
    {
	return 0;
    }
}

rb_wrap(static JEMALLOC_UNUSED, cache_tree_, AllocationCache,
    AllocationCacheRecord, link, cacheComp)

// Parse utrace records.  Following are prototypical examples of each type of
// record:
//
//  <jemalloc>:utrace: 31532 malloc_init()
//  <jemalloc>:utrace: 31532 0x800b01000 = malloc(34816)
//  <jemalloc>:utrace: 31532 free(0x800b0a400)
//  <jemalloc>:utrace: 31532 0x800b35230 = realloc(0x800b35230, 45)
Trace *
parseInput(FILE *infile, uint64_t pastEvent)
{
    Trace *rVal;
    uint64_t maxLen;
    int result;
    char buf[128];
    unsigned pid;
    void *addr, *oldAddr;
    size_t size;

    rVal = (Trace *) malloc(sizeof(Trace));
    if (rVal == NULL)
    {
	fprintf(stderr, "mtrgraph: Error in malloc()\n");
	goto RETURN;
    }

    maxLen = 1024;
    rVal->trace = (TraceRecord *) malloc(maxLen * sizeof(TraceRecord));
    if (rVal->trace == NULL)
    {
	fprintf(stderr, "mtrgraph: Error in malloc()\n");
	free(rVal);
	rVal = NULL;
	goto RETURN;
    }

    rVal->traceLen = 0;
    while (rVal->traceLen < pastEvent)
    {
	// Expand trace buffer, if necessary.
	if (rVal->traceLen == maxLen)
	{
	    TraceRecord *t;

	    maxLen *= 2;
	    t = (TraceRecord *) realloc(rVal->trace,
					maxLen * sizeof(TraceRecord));
	    if (t == NULL)
	    {
		fprintf(stderr, "mtrgraph: Error in realloc()\n");
		free(rVal->trace);
		free(rVal);
		rVal = NULL;
		goto RETURN;
	    }
	    rVal->trace = t;
	}

	// Get a line of input.
	{
	    int c;
	    unsigned i;

	    for (i = 0; i < sizeof(buf) - 1; i++)
	    {
		c = fgetc(infile);
		switch (c)
		{
		    case EOF:
		    {
			goto RETURN;
		    }
		    case '\n':
		    {
			buf[i] = '\0';
			goto OUT;
		    }
		    default:
		    {
		        buf[i] = c;
			break;
		    }
		}
	    }
	    OUT:;
	}

	// realloc?
	result = sscanf(buf, "<jemalloc>:utrace: %u %p = realloc(%p, %zu)",
	                &pid, &addr, &oldAddr, &size);
	if (result == 4)
	{
		rVal->trace[rVal->traceLen].oldAddr = (uintptr_t) oldAddr;
		rVal->trace[rVal->traceLen].size = size;
		rVal->trace[rVal->traceLen].addr = (uintptr_t) addr;

		rVal->traceLen++;
		continue;
	}

	// malloc?
	result = sscanf(buf, "<jemalloc>:utrace: %u %p = malloc(%zu)",
	                &pid, &addr, &size);
	if (result == 3)
	{
		rVal->trace[rVal->traceLen].oldAddr = (uintptr_t) NULL;
		rVal->trace[rVal->traceLen].size = size;
		rVal->trace[rVal->traceLen].addr = (uintptr_t) addr;

		rVal->traceLen++;
		continue;
	}

	// free?
	result = sscanf(buf, "<jemalloc>:utrace: %u free(%p)",
	                &pid, &oldAddr);
	if (result == 2)
	{
		rVal->trace[rVal->traceLen].oldAddr = (uintptr_t) oldAddr;
		rVal->trace[rVal->traceLen].size = 0;
		rVal->trace[rVal->traceLen].addr = (uintptr_t) NULL;

		rVal->traceLen++;
		continue;
	}

	// malloc_init?
	result = sscanf(buf, "<jemalloc>:utrace: %u malloc_init()",
	                &pid);
	if (result == 1)
	{
		continue;
	}

	goto ERROR;
    }

RETURN:
    return rVal;
ERROR:
    fprintf(stderr, "mtrgraph: Error reading record %"PRIu64" of input\n",
	    rVal->traceLen + 1);
    free(rVal->trace);
    free(rVal);
    rVal = NULL;
    goto RETURN;
}

bool
genOutput(FILE *outfile, const char *fileType, bool legend,
	  unsigned long xSize, unsigned long ySize,
	  Trace *trace, uint64_t minAddr, uint64_t maxAddr, uint64_t quantum,
	  uint64_t minEvent, uint64_t pastEvent, uint64_t stride)
{
    bool rVal;
    gdImagePtr img;
#define NCOLORS 256
    int white, black;
    int colors[NCOLORS];
    uint64_t i, buckets[ySize];
    unsigned long bucket;
    unsigned long x = 0;
    AllocationCache cache;
    AllocationCacheRecord *rec, key;
    gdFontPtr font;

    img = gdImageCreate((int) xSize, (int) ySize);

    black = gdImageColorAllocate(img, 0, 0, 0);
    white = gdImageColorAllocate(img, 255, 255, 255);
    // Create a palette of colors.
    for (i = 0; i < NCOLORS; i++)
    {
	colors[i] = gdImageColorAllocate(img, 255 - i, i, 
		(i < NCOLORS / 2) ? i * 2 : (NCOLORS - i - 1) * 2);
    }

    // Set up fonts.
    font = gdFontGetLarge();

    memset(buckets, 0, ySize * sizeof(uint64_t));

    cache_tree_new(&cache);

    for (i = 0; i < trace->traceLen; i++)
    {
	if (trace->trace[i].oldAddr == 0 && trace->trace[i].addr != 0)
	{
	    // malloc.

	    // Update buckets.
	    if (trace->trace[i].size > 0)
	    {
		uint64_t size, offset;

		size = trace->trace[i].size;
		bucket = (trace->trace[i].addr - minAddr) / quantum;
		offset =  (trace->trace[i].addr - minAddr) % quantum;

		if (bucket < ySize)
		{
		    if (quantum - offset >= size)
		    {
			buckets[bucket] += size;
			size = 0;
		    }
		    else
		    {
			buckets[bucket] += (quantum - offset);
			size -= (quantum - offset);
		    }
		    bucket++;

		    while (bucket < ySize && size > 0)
		    {
			if (size > quantum)
			{
			    buckets[bucket] += quantum;
			    size -= quantum;
			}
			else
			{
			    buckets[bucket] += size;
			    size = 0;
			}
			bucket++;
		    }
		}
	    }

	    // Cache size of allocation.
	    rec = (AllocationCacheRecord *)
		  malloc(sizeof(AllocationCacheRecord));
	    if (rec == NULL)
	    {
		fprintf(stderr, "mtrgraph: Error in malloc()\n");
		rVal = true;
		goto RETURN;
	    }

	    rec->addr = trace->trace[i].addr;
	    rec->size = trace->trace[i].size;

	    cache_tree_insert(&cache, rec);
	}
	else if (trace->trace[i].oldAddr != 0 && trace->trace[i].addr != 0)
	{
	    // realloc.

	    // Remove old allocation from cache.
	    key.addr = trace->trace[i].oldAddr;
	    rec = cache_tree_search(&cache, &key);
	    if (rec == NULL)
	    {
		fprintf(stderr,
			"mtrgraph: Trace record %"PRIu64
			" realloc()s unknown object 0x%"PRIx64"\n",
			i, trace->trace[i].oldAddr);
		rVal = true;
		goto RETURN;
	    }

	    // Update buckets (dealloc).
	    if (rec->size > 0)
	    {
		uint64_t size, offset;

		size = rec->size;
		bucket = (trace->trace[i].oldAddr - minAddr) / quantum;
		offset =  (trace->trace[i].oldAddr - minAddr) % quantum;

		if (bucket < ySize)
		{
		    if (quantum - offset >= size)
		    {
			buckets[bucket] -= size;
			size = 0;
		    }
		    else
		    {
			buckets[bucket] -= (quantum - offset);
			size -= (quantum - offset);
		    }
		    bucket++;

		    while (bucket < ySize && size > 0)
		    {
			if (size > quantum)
			{
			    buckets[bucket] -= quantum;
			    size -= quantum;
			}
			else
			{
			    buckets[bucket] -= size;
			    size = 0;
			}
			bucket++;
		    }
		}
	    }

	    // Update buckets (alloc).
	    if (trace->trace[i].size > 0)
	    {
		uint64_t size, offset;

		size = trace->trace[i].size;
		bucket = (trace->trace[i].addr - minAddr) / quantum;
		offset =  (trace->trace[i].addr - minAddr) % quantum;

		if (bucket < ySize)
		{
		    if (quantum - offset >= size)
		    {
			buckets[bucket] += size;
			size = 0;
		    }
		    else
		    {
			buckets[bucket] += (quantum - offset);
			size -= (quantum - offset);
		    }
		    bucket++;

		    while (bucket < ySize && size > 0)
		    {
			if (size > quantum)
			{
			    buckets[bucket] += quantum;
			    size -= quantum;
			}
			else
			{
			    buckets[bucket] += size;
			    size = 0;
			}
			bucket++;
		    }
		}
	    }

	    // Cache size of allocation.
	    cache_tree_remove(&cache, rec);
	    rec->addr = trace->trace[i].addr;
	    rec->size = trace->trace[i].size;
	    cache_tree_insert(&cache, rec);
	}
	else if (trace->trace[i].oldAddr != 0
		 && trace->trace[i].size == 0
		 && trace->trace[i].addr == 0)
	{
	    // free.

	    // Remove old allocation from cache.
	    key.addr = trace->trace[i].oldAddr;
	    rec = cache_tree_search(&cache, &key);
	    if (rec == NULL)
	    {
		fprintf(stderr,
			"mtrgraph: Trace record %"PRIu64
			" free()s unknown object 0x%"PRIx64"\n",
			i, trace->trace[i].oldAddr);
		rVal = true;
		goto RETURN;
	    }

	    // Update buckets.
	    if (rec->size > 0)
	    {
		uint64_t size, offset;

		size = rec->size;
		bucket = (trace->trace[i].oldAddr - minAddr) / quantum;
		offset =  (trace->trace[i].oldAddr - minAddr) % quantum;

		if (bucket < ySize)
		{
		    if (quantum - offset >= size)
		    {
			buckets[bucket] -= size;
			size = 0;
		    }
		    else
		    {
			buckets[bucket] -= (quantum - offset);
			size -= (quantum - offset);
		    }
		    bucket++;

		    while (bucket < ySize && size > 0)
		    {
			if (size > quantum)
			{
			    buckets[bucket] -= quantum;
			    size -= quantum;
			}
			else
			{
			    buckets[bucket] -= size;
			    size = 0;
			}
			bucket++;
		    }
		}
	    }

	    cache_tree_remove(&cache, rec);
	    free(rec);
	}

	// Plot buckets in graph.
	if (i >= minEvent && i < pastEvent && ((i - minEvent) % stride) == 0)
	{
	    unsigned long j;
	    int color;

	    for (j = 0; j < ySize; j++)
	    {
		if (buckets[j] > 0)
		{
		    color = (NCOLORS * ((double) buckets[j] / (double) quantum))
			    - 1;
		    gdImageSetPixel(img, x, ySize - j, colors[color]);
		}
	    }
	    x++;
	}
    }

    // Print graph legend.
    if (legend)
    {
#define BUFLEN 256
	char buf[BUFLEN];

	// Create color palette legend.
	if (ySize >= NCOLORS)
	{
	    for (i = 0; i < NCOLORS; i++)
	    {
		gdImageLine(img, 0, NCOLORS - i - 1, 31, NCOLORS - i - 1,
			    colors[i]);
	    }
	    gdImageLine(img, 0, 0, 31, 0, white);
	    gdImageLine(img, 0, 256, 31, 256, white);
	    gdImageLine(img, 0, 0, 0, 256, white);
	    gdImageLine(img, 31, 0, 31, 256, white);

	    gdImageString(img, font, 40, 0, (unsigned char *)"Full bucket",
	        white);
	    gdImageString(img, font, 40, 240,
	        (unsigned char *)"Fragmented bucket", white);
	}

	snprintf(buf, BUFLEN,
		 "Horizontal: Events [%"PRIu64"..%"PRIu64"), stride %"PRIu64"",
		 minEvent, pastEvent, stride);
	gdImageString(img, font, 200, 0, (unsigned char *)buf, white);

	snprintf(buf, BUFLEN,
		 "Vertical: Addresses [0x%016"PRIx64"..0x%016"PRIx64
		 "), bucket size %"PRIu64"", minAddr, maxAddr, quantum);
	gdImageString(img, font, 200, 20, (unsigned char *)buf, white);

	snprintf(buf, BUFLEN,
		 "Graph dimensions: %lu events by %lu buckets",
		 xSize, ySize);
	gdImageString(img, font, 200, 40, (unsigned char *)buf, white);
    }

    if (strcmp(fileType, "png") == 0)
    {
	gdImagePng(img, outfile);
    }
    else if (strcmp(fileType, "jpg") == 0)
    {
	gdImageJpeg(img, outfile, 100);
    }
    else if (strcmp(fileType, "gif") == 0)
    {
	gdImageGif(img, outfile);
    }
    else
    {
	// Unreachable code.
	fprintf(stderr, "mtrgraph: Unsupported output file type '%s'\n",
		fileType);
	rVal = true;
	goto RETURN;
    }

    rVal = false;
RETURN:
    // Clean up cache.
    while (true)
    {
	rec = cache_tree_first(&cache);
	if (rec == NULL)
	{
	    break;
	}

	cache_tree_remove(&cache, rec);
	free(rec);
    }
    gdImageDestroy(img);
    return rVal;
}

void
usage(FILE *fp)
{
    fprintf(fp, "mtrgraph usage:\n");
    fprintf(fp, "    mtrgraph -h\n");
    fprintf(fp, "    mtrgraph [<options>]\n");
    fprintf(fp, "\n");
    fprintf(fp, "    Option     | Description\n");
    fprintf(fp, "    -----------+------------------------------------------\n");
    fprintf(fp, "    -h         | Print usage and exit.\n");
    fprintf(fp, "    -n         | Don't actually generate a graph.\n");
    fprintf(fp, "    -q         | Don't print statistics to stdout.\n");
    fprintf(fp, "    -l         | Don't generate legend in graph.\n");
    fprintf(fp, "    -f <file>  | Input filename.\n");
    fprintf(fp, "    -o <file>  | Output filename.\n");
    fprintf(fp, "    -t <type>  | Output file type (png*, gif, jpg).\n");
    fprintf(fp, "    -x <size>  | Horizontal size of graph area, in pixels.\n");
    fprintf(fp, "    -y <size>  | Vertical size of graph area, in pixels.\n");
    fprintf(fp, "    -m <addr>  | Minimum address to graph.\n");
    fprintf(fp, "    -M <addr>  | Maximum address to graph.\n");
    fprintf(fp, "    -e <time>  | First event to graph.\n");
    fprintf(fp, "    -E <time>  | Last event to graph.\n");
    fprintf(fp, "    -s <count> | Stride between event samples.\n");
}

int
main(int argc, char **argv)
{
    int rVal, c;
    bool optNoAct = false;
    bool optQuiet = false;
    bool optLegend = true;
    const char *optInfile = NULL;
    const char *optOutfile = NULL;
    const char *optFileType = "gif";
    unsigned long optXSize = 1280;
    unsigned long optYSize = 1024;
    uint64_t optMinAddr = 0ULL;
    uint64_t optMaxAddr = ULLONG_MAX;
    uint64_t optMinEvent = 0ULL;
    uint64_t optPastEvent = ULLONG_MAX;
    uint64_t optStride = 0ULL;

    FILE *infile, *outfile;
    uint64_t minAddr, maxAddr;
    Trace *trace = NULL;
    uint64_t i, quantum, stride;

    opterr = 0;
    optind = 1;
    while ((c = getopt(argc, argv,
#ifdef _GNU_SOURCE
		       // Without this, glibc will permute unknown options to
		       // the end of the argument list.
		       "+"
#endif
		       "hnqlf:o:t:x:y:m:M:e:E:s:")) != -1)
    {
	switch (c)
	{
	    case 'h':
	    {
		if (argc != 2)
		{
		    fprintf(stderr, 
			    "mtrgraph: Incorrect number of arguments\n");
		    usage(stderr);
		    rVal = 1;
		    goto RETURN;
		}
		usage(stdout);
		rVal = 0;
		goto RETURN;
	    }
	    case 'n':
	    {
		optNoAct = true;
		break;
	    }
	    case 'q':
	    {
		optQuiet = true;
		break;
	    }
	    case 'l':
	    {
		optLegend = false;
		break;
	    }
	    case 'f':
	    {
		optInfile = optarg;
		break;
	    }
	    case 'o':
	    {
		optOutfile = optarg;
		break;
	    }
	    case 't':
	    {
		if (strcmp(optarg, "png")
		    && strcmp(optarg, "jpg")
		    && strcmp(optarg, "gif"))
		{
		    fprintf(stderr, "mtrgraph: Invalid option '-%c %s'\n",
			    c, optarg);
		    usage(stderr);
		    rVal = 1;
		    goto RETURN;
		}
		optFileType = optarg;
		break;
	    }
	    case 'x':
	    {
		errno = 0;
		optXSize = strtoul(optarg, NULL, 0);
		if (optXSize == ULONG_MAX && errno != 0)
		{
		    fprintf(stderr, "mtrgraph: Error in option '-%c %s': %s\n",
			    c, optarg, strerror(errno));
		    usage(stderr);
		    rVal = 1;
		    goto RETURN;
		}
		else if (optXSize == 0)
		{
		    fprintf(stderr, "mtrgraph: Invalid option '-%c %s'\n",
			    c, optarg);
		    usage(stderr);
		    rVal = 1;
		    goto RETURN;
		}
		break;
	    }
	    case 'y':
	    {
		errno = 0;
		optYSize = strtoul(optarg, NULL, 0);
		if (optYSize == ULONG_MAX && errno != 0)
		{
		    fprintf(stderr, "mtrgraph: Error in option '-%c %s': %s\n",
			    c, optarg, strerror(errno));
		    usage(stderr);
		    rVal = 1;
		    goto RETURN;
		}
		else if (optYSize == 0)
		{
		    fprintf(stderr, "mtrgraph: Invalid option '-%c %s'\n",
			    c, optarg);
		    usage(stderr);
		    rVal = 1;
		    goto RETURN;
		}
		break;
	    }
	    case 'm':
	    {
		errno = 0;
		optMinAddr = strtoull(optarg, NULL, 0);
		if (optMinAddr == ULLONG_MAX && errno != 0)
		{
		    fprintf(stderr, "mtrgraph: Error in option '-%c %s': %s\n",
			    c, optarg, strerror(errno));
		    usage(stderr);
		    rVal = 1;
		    goto RETURN;
		}
		else if (optMinAddr == 0)
		{
		    fprintf(stderr, "mtrgraph: Invalid option '-%c %s'\n",
			    c, optarg);
		    usage(stderr);
		    rVal = 1;
		    goto RETURN;
		}
		break;
	    }
	    case 'M':
	    {
		errno = 0;
		optMaxAddr = strtoull(optarg, NULL, 0);
		if (optMaxAddr == ULLONG_MAX && errno != 0)
		{
		    fprintf(stderr, "mtrgraph: Error in option '-%c %s': %s\n",
			    c, optarg, strerror(errno));
		    usage(stderr);
		    rVal = 1;
		    goto RETURN;
		}
		else if (optMaxAddr == 0)
		{
		    fprintf(stderr, "mtrgraph: Invalid option '-%c %s'\n",
			    c, optarg);
		    usage(stderr);
		    rVal = 1;
		    goto RETURN;
		}
		break;
	    }
	    case 'e':
	    {
		errno = 0;
		optMinEvent = strtoull(optarg, NULL, 0);
		if (optMinEvent == ULLONG_MAX && errno != 0)
		{
		    fprintf(stderr, "mtrgraph: Error in option '-%c %s': %s\n",
			    c, optarg, strerror(errno));
		    usage(stderr);
		    rVal = 1;
		    goto RETURN;
		}
		else if (optMinEvent == 0)
		{
		    fprintf(stderr, "mtrgraph: Invalid option '-%c %s'\n",
			    c, optarg);
		    usage(stderr);
		    rVal = 1;
		    goto RETURN;
		}
		break;
	    }
	    case 'E':
	    {
		errno = 0;
		optPastEvent = strtoull(optarg, NULL, 0);
		if (optPastEvent == ULLONG_MAX && errno != 0)
		{
		    fprintf(stderr, "mtrgraph: Error in option '-%c %s': %s\n",
			    c, optarg, strerror(errno));
		    usage(stderr);
		    rVal = 1;
		    goto RETURN;
		}
		else if (optPastEvent == 0)
		{
		    fprintf(stderr, "mtrgraph: Invalid option '-%c %s'\n",
			    c, optarg);
		    usage(stderr);
		    rVal = 1;
		    goto RETURN;
		}
		break;
	    }
	    case 's':
	    {
		errno = 0;
		optStride = strtoull(optarg, NULL, 0);
		if (optStride == ULLONG_MAX && errno != 0)
		{
		    fprintf(stderr, "mtrgraph: Error in option '-%c %s': %s\n",
			    c, optarg, strerror(errno));
		    usage(stderr);
		    rVal = 1;
		    goto RETURN;
		}
		else if (optStride == 0)
		{
		    fprintf(stderr, "mtrgraph: Invalid option '-%c %s'\n",
			    c, optarg);
		    usage(stderr);
		    rVal = 1;
		    goto RETURN;
		}
		break;
	    }
	    default:
	    {
		fprintf(stderr, "mtrgraph: Unrecognized option\n");
		usage(stderr);
		rVal = 1;
		goto RETURN;
	    }
	}
    }

    if (optInfile == NULL)
    {
	infile = stdin;
    }
    else
    {
	infile = fopen(optInfile, "r");
	if (infile == NULL)
	{
	    fprintf(stderr, "mtrgraph: Invalid option '-f %s': %s\n",
		    optInfile, strerror(errno));
	    usage(stderr);
	    rVal = 1;
	    goto RETURN;
	}
    }

    if (optNoAct == false)
    {
	if (optOutfile == NULL)
	{
	    outfile = stdout;
	}
	else
	{
	    outfile = fopen(optOutfile, "w");
	    if (outfile == NULL)
	    {
		fprintf(stderr, "mtrgraph: Invalid option '-o %s': %s\n",
			optOutfile, strerror(errno));
		usage(stderr);
		rVal = 1;
		goto RETURN;
	    }
	}
    }
    else
    {
	outfile = NULL;
    }

    // Parse input trace.
    if ((trace = parseInput(infile, optPastEvent)) == NULL)
    {
	rVal = 1;
	goto RETURN;
    }

    // Calculate graph parameters.

    // stride.
    if (optMinEvent >= trace->traceLen || optPastEvent <= optMinEvent)
    {
	fprintf(stderr, "mtrgraph: No events in range [%"PRIu64"..%"PRIu64")\n",
		optMinEvent, optPastEvent);
	rVal = 1;
	goto RETURN;
    }

    if (optPastEvent >= trace->traceLen)
    {
	optPastEvent = trace->traceLen;
    }
    stride = (optPastEvent - optMinEvent) / optXSize;
    if ((optPastEvent - optMinEvent) % optXSize != 0)
    {
	stride++;
    }
    if (stride < optStride)
    {
	stride = optStride;
    }

    // Contract optXSize if there is unused space at the end of the graph.
    {
	unsigned long xSize;

	xSize = (optPastEvent - optMinEvent) / stride;
	if ((optPastEvent - optMinEvent) % stride != 0)
	{
	    xSize++;
	}

	if (optXSize > xSize)
	{
	    optXSize = xSize;
	}
    }

    // minAddr and maxAddr.
    minAddr = ULLONG_MAX;
    maxAddr = 0;
    for (i = 0; i < trace->traceLen; i++)
    {
	if (i >= optMinEvent && i < optPastEvent)
	{
	    if (trace->trace[i].addr != 0)
	    {
		if (trace->trace[i].addr >= optMinAddr
		    && trace->trace[i].addr < minAddr)
		{
		    minAddr = trace->trace[i].addr;
		}

		if (trace->trace[i].addr < optMaxAddr
		    && trace->trace[i].addr + trace->trace[i].size > maxAddr)
		{
		    if (trace->trace[i].addr + trace->trace[i].size 
			< optMaxAddr)
		    {
			maxAddr = trace->trace[i].addr + trace->trace[i].size;
		    }
		    else
		    {
			maxAddr = optMaxAddr;
		    }
		}
	    }
	}
    }
    if (minAddr == ULLONG_MAX || maxAddr == 0 || minAddr >= maxAddr)
    {
	fprintf(stderr,
		"mtrgraph: No events in specified address range "
		"0x%016"PRIx64"..0x%016"PRIx64"\n",
		minAddr, maxAddr);
	rVal = 1;
	goto RETURN;
    }

    // quantum.
    for (quantum = 1;
	((double) (maxAddr - minAddr) / (double) quantum) > (double) optYSize;
	 quantum <<= 1)
    {
	// Do nothing.
    }

    // Expand minAddr and maxAddr to the nearest quantum boundaries.
    minAddr -= (minAddr % quantum);
    if (maxAddr % quantum != 0)
    {
	maxAddr -= (maxAddr % quantum);
	maxAddr += quantum;
    }
    if (minAddr >= maxAddr)
    {
	fprintf(stderr,
		"mtrgraph: No events in specified address range "
		"0x%016"PRIx64"..0x%016"PRIx64"\n",
		minAddr, maxAddr);
	rVal = 1;
	goto RETURN;
    }

    if (optQuiet == false)
    {
	fprintf(stderr, "%"PRIu64" records read\n", trace->traceLen);
	fprintf(stderr, "Graph addresses [0x%016"PRIx64"..0x%016"PRIx64")\n",
		minAddr, maxAddr);
	fprintf(stderr, "Graph events [%"PRIu64"..%"PRIu64")\n",
		optMinEvent, optPastEvent);
	fprintf(stderr, "Quantum: %"PRIu64"\n", quantum);
	fprintf(stderr, "Stride: %"PRIu64"\n", stride);
	fprintf(stderr, "Graph size: %lu x %lu\n", optXSize, optYSize);
    }

    // Contract optYSize if there is unused image space.
    if (optYSize > (maxAddr - minAddr) / quantum)
    {
	optYSize = (maxAddr - minAddr) / quantum;
    }

    // Generate graph.
    if (optNoAct == false)
    {
	if (genOutput(outfile, optFileType, optLegend, optXSize, optYSize,
		      trace, minAddr, maxAddr, quantum, 
		      optMinEvent, optPastEvent, stride))
	{
	    rVal = 1;
	    goto RETURN;
	}
    }

    rVal = 0;
RETURN:
    if (trace != NULL)
    {
	if (trace->trace != NULL)
	{
	    free(trace->trace);
	}
	free(trace);
    }
    return rVal;
}
