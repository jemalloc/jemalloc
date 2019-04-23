#ifndef JEMALLOC_INTERNAL_ARENA_STRUCTS_A_H
#define JEMALLOC_INTERNAL_ARENA_STRUCTS_A_H

#include "jemalloc/internal/bitmap.h"
#include "jemalloc/internal/ql.h"

struct arena_slab_data_s {
	struct {
		/* Per region allocated/deallocated bitmap. */
		union {
			bitmap_t	full_bitmap[BITMAP_GROUPS_MAX];
			struct {
				bitmap_t		bitmap;
				ql_elm(extent_t)        ql_link;
			} mesh_data;
		};
	} internal;
};

#endif /* JEMALLOC_INTERNAL_ARENA_STRUCTS_A_H */
