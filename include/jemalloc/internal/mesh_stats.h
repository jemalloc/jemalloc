#ifndef JEMALLOC_INTERNAL_MESH_STATS_H
#define JEMALLOC_INTERNAL_MESH_STATS_H

typedef struct mesh_bin_stats_s mesh_bin_stats_t;
struct mesh_bin_stats_s {
        unsigned        shape_counts[1 << 8];
};

#endif /* JEMALLOC_INTERNAL_MESH_STATS_H */
