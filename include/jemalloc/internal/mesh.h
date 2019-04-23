#ifndef JEMALLOC_INTERNAL_MESH_H
#define JEMALLOC_INTERNAL_MESH_H

#include "jemalloc/internal/mesh_stats.h"

extern bool opt_mesh;

typedef struct mesh_bin_data_s mesh_bin_data_t;
struct mesh_bin_data_s {
	extent_list_t		shape_table[1 << 8];
	mesh_bin_stats_t	stats;
};

typedef struct mesh_bin_datas_s mesh_bin_datas_t;
struct mesh_bin_datas_s {
	mesh_bin_data_t *bin_data_shards;
};

typedef struct mesh_arena_data_s mesh_arena_data_t;
struct mesh_arena_data_s {
	mesh_bin_datas_t *bin_datas;
};

bool mesh_binind_meshable(szind_t bindind);
bool mesh_slab_is_candidate(extent_t *slab);
void mesh_slab_shape_add(mesh_arena_data_t *data, arena_slab_data_t *slab_data,
    const bin_info_t *bin_info, extent_t *slab);
void mesh_slab_shape_remove(mesh_arena_data_t *data,
    arena_slab_data_t *slab_data, const bin_info_t *bin_info, extent_t *slab);
mesh_arena_data_t * mesh_arena_data_new(tsdn_t *tsdn, base_t *base);
bool mesh_boot(void);

/* Stats. */
void mesh_bin_stats_merge(tsdn_t *tsdn, mesh_bin_stats_t *dst_mesh_bin_stats,
    mesh_arena_data_t *mesh_arena_data, bin_t *bin, szind_t binind,
    unsigned binshard);

#endif /* JEMALLOC_INTERNAL_MESH_H */
