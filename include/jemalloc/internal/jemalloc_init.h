#ifndef JEMALLOC_INTERNAL_JEMALLOC_INIT_H
#define JEMALLOC_INTERNAL_JEMALLOC_INIT_H

enum malloc_init_e {
	malloc_init_uninitialized = 3,
	malloc_init_a0_initialized = 2,
	malloc_init_recursible = 1,
	malloc_init_initialized = 0 /* Common case --> jnz. */
};
typedef enum malloc_init_e malloc_init_t;

extern malloc_init_t malloc_init_state;

bool malloc_is_initializer(void);
bool malloc_initializer_is_set(void);
void malloc_initializer_set(void);

bool malloc_init_hard_a0(void);
bool malloc_init_hard(void);

JEMALLOC_ALWAYS_INLINE bool
malloc_init_a0(void) {
	if (unlikely(malloc_init_state == malloc_init_uninitialized)) {
		return malloc_init_hard_a0();
	}
	return false;
}

JEMALLOC_ALWAYS_INLINE bool
malloc_initialized(void) {
	return (malloc_init_state == malloc_init_initialized);
}

JEMALLOC_ALWAYS_INLINE bool
malloc_init(void) {
	if (unlikely(!malloc_initialized()) && malloc_init_hard()) {
		return true;
	}
	return false;
}

#endif /* JEMALLOC_INTERNAL_JEMALLOC_INIT_H */
