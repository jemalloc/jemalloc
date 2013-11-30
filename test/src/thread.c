#include "test/jemalloc_test.h"

#ifdef _WIN32
void
je_thread_create(je_thread_t *thread, void *(*proc)(void *), void *arg)
{
	LPTHREAD_START_ROUTINE routine = (LPTHREAD_START_ROUTINE)proc;
	*thread = CreateThread(NULL, 0, routine, arg, 0, NULL);
	if (*thread == NULL)
		test_fail("Error in CreateThread()\n");
}

void
je_thread_join(je_thread_t thread, void **ret)
{

	WaitForSingleObject(thread, INFINITE);
}

#else
void
je_thread_create(je_thread_t *thread, void *(*proc)(void *), void *arg)
{

	if (pthread_create(thread, NULL, proc, arg) != 0)
		test_fail("Error in pthread_create()\n");
}

void
je_thread_join(je_thread_t thread, void **ret)
{

	pthread_join(thread, ret);
}
#endif
