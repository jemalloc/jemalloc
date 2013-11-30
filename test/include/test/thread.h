
/* Abstraction layer for threading in tests */
#ifdef _WIN32
#include <windows.h>
typedef HANDLE je_thread_t;
#else
#include <pthread.h>
typedef pthread_t je_thread_t;
#endif

void	je_thread_create(je_thread_t *thread, void *(*proc)(void *), void *arg);
void	je_thread_join(je_thread_t thread, void **ret);
