#ifndef JEMALLOC_INTERNAL_JEMALLOC_FORK_H
#define JEMALLOC_INTERNAL_JEMALLOC_FORK_H

void jemalloc_prefork(void);
void jemalloc_postfork_parent(void);
void jemalloc_postfork_child(void);

#endif /* JEMALLOC_INTERNAL_JEMALLOC_FORK_H */
