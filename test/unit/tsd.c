#include "test/jemalloc_test.h"

#define THREAD_DATA 0x72b65c10

typedef unsigned int data_t;

void
data_cleanup(void *arg)
{
	data_t *data = (data_t *)arg;

	malloc_printf("Cleanup for data %x.\n", *data);
}

malloc_tsd_protos(, data, data_t)
malloc_tsd_externs(data, data_t)
#define DATA_INIT 0x12345678
malloc_tsd_data(, data, data_t, DATA_INIT)
malloc_tsd_funcs(, data, data_t, DATA_INIT, data_cleanup)

void *
je_thread_start(void *arg)
{
	data_t d = (data_t)(uintptr_t) arg;
	malloc_printf("Initial tsd_get returns %x. Expected %x.\n",
		*data_tsd_get(), DATA_INIT);

	data_tsd_set(&d);
	malloc_printf("After tsd_set: %x. Expected %x.\n",
		*data_tsd_get(), d);

	d = 0;
	malloc_printf("After resetting local data: %x. Expected %x.\n",
		*data_tsd_get(), (data_t)(uintptr_t) arg);

	return NULL;
}

int
main(void)
{
	je_thread_t thread;

	malloc_printf("Test begin\n");

	data_tsd_boot();
	je_thread_start((void *) 0xa5f3e329);

	je_thread_create(&thread, je_thread_start, (void *) THREAD_DATA);
	je_thread_join(thread, NULL);

	malloc_printf("Test end\n");
	return (0);
}
