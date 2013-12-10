/* Abstraction layer for threading in tests */
#ifdef _WIN32
typedef HANDLE je_thread_t;
#else
typedef pthread_t je_thread_t;
#endif

void	je_thread_create(je_thread_t *thread, void *(*proc)(void *), void *arg);
void	je_thread_join(je_thread_t thread, void **ret);
