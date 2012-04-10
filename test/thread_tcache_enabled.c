#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include <assert.h>
#include <errno.h>

#define	JEMALLOC_MANGLE
#include "jemalloc_test.h"

void *
thread_start(void *arg)
{
	int err;
	size_t sz;
	bool e0, e1;

	sz = sizeof(bool);
	if ((err = mallctl("thread.tcache.enabled", &e0, &sz, NULL, 0))) {
		if (err == ENOENT) {
#ifdef JEMALLOC_TCACHE
			assert(false);
#endif
		}
		goto label_return;
	}

	if (e0) {
		e1 = false;
		assert(mallctl("thread.tcache.enabled", &e0, &sz, &e1, sz)
		    == 0);
		assert(e0);
	}

	e1 = true;
	assert(mallctl("thread.tcache.enabled", &e0, &sz, &e1, sz) == 0);
	assert(e0 == false);

	e1 = true;
	assert(mallctl("thread.tcache.enabled", &e0, &sz, &e1, sz) == 0);
	assert(e0);

	e1 = false;
	assert(mallctl("thread.tcache.enabled", &e0, &sz, &e1, sz) == 0);
	assert(e0);

	e1 = false;
	assert(mallctl("thread.tcache.enabled", &e0, &sz, &e1, sz) == 0);
	assert(e0 == false);

	free(malloc(1));
	e1 = true;
	assert(mallctl("thread.tcache.enabled", &e0, &sz, &e1, sz) == 0);
	assert(e0 == false);

	free(malloc(1));
	e1 = true;
	assert(mallctl("thread.tcache.enabled", &e0, &sz, &e1, sz) == 0);
	assert(e0);

	free(malloc(1));
	e1 = false;
	assert(mallctl("thread.tcache.enabled", &e0, &sz, &e1, sz) == 0);
	assert(e0);

	free(malloc(1));
	e1 = false;
	assert(mallctl("thread.tcache.enabled", &e0, &sz, &e1, sz) == 0);
	assert(e0 == false);

	free(malloc(1));
label_return:
	return (NULL);
}

int
main(void)
{
	int ret = 0;
	pthread_t thread;

	fprintf(stderr, "Test begin\n");

	thread_start(NULL);

	if (pthread_create(&thread, NULL, thread_start, NULL)
	    != 0) {
		fprintf(stderr, "%s(): Error in pthread_create()\n", __func__);
		ret = 1;
		goto label_return;
	}
	pthread_join(thread, (void *)&ret);

	thread_start(NULL);

	if (pthread_create(&thread, NULL, thread_start, NULL)
	    != 0) {
		fprintf(stderr, "%s(): Error in pthread_create()\n", __func__);
		ret = 1;
		goto label_return;
	}
	pthread_join(thread, (void *)&ret);

	thread_start(NULL);

label_return:
	fprintf(stderr, "Test end\n");
	return (ret);
}
