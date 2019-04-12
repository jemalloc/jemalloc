#define JEMALLOC_MESH_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/mesh.h"

bool opt_mesh = false;

unsigned nmeshable_scs;
unsigned nmeshable_bins_per_arena;
unsigned binind_to_meshind_table[SC_NBINS];
// (SC_NBINS - nmeshable_scs) unused at end
unsigned meshind_to_binind_table[SC_NBINS];

bool
mesh_binind_meshable(szind_t binind) {
	assert(binind < SC_NBINS);
	unsigned meshind = binind_to_meshind_table[binind];
	assert(meshind < nmeshable_scs || meshind == SC_NBINS);
	return meshind != SC_NBINS;
}

bool
mesh_slab_is_candidate(extent_t *slab) {
	szind_t binind = extent_szind_get(slab);
	return mesh_binind_meshable(binind);
}

static void
insert_into_bin_data(mesh_bin_data_t *bin_data, uint8_t key, extent_t *slab) {
	if (config_stats) {
		bin_data->stats.shape_counts[key]++;
	}
}

static void
remove_from_bin_data(mesh_bin_data_t *bin_data, uint8_t key, extent_t *slab) {
	if (config_stats) {
		assert(bin_data->stats.shape_counts[key] != 0);
		bin_data->stats.shape_counts[key]--;
	}
}

static mesh_bin_data_t *
get_bin_data_for_slab(mesh_arena_data_t *data, const bin_info_t *bin_info,
    extent_t *slab) {
	szind_t binind = extent_szind_get(slab);
	unsigned meshind = binind_to_meshind_table[binind];
	assert(meshind != SC_NBINS);
	unsigned shard = extent_binshard_get(slab);
	return &data->bin_datas[meshind].bin_data_shards[shard];
}

void
mesh_slab_shape_add(mesh_arena_data_t *data, arena_slab_data_t *slab_data,
    const bin_info_t *bin_info, extent_t *slab) {
	assert(!bitmap_full(slab_data->bitmap, &bin_info->bitmap_info));
	assert(extent_nfree_get(slab) != bin_info->nregs);

	mesh_bin_data_t *bin_data = get_bin_data_for_slab(data, bin_info, slab);
	uint8_t key = bitmap_get_first_logical_byte(slab_data->bitmap,
	    &bin_info->bitmap_info);
	insert_into_bin_data(bin_data, key, slab);
}

void
mesh_slab_shape_remove(mesh_arena_data_t *data, arena_slab_data_t *slab_data,
    const bin_info_t *bin_info, extent_t *slab) {
	assert(!bitmap_full(slab_data->bitmap, &bin_info->bitmap_info));
	assert(extent_nfree_get(slab) != bin_info->nregs);

	mesh_bin_data_t *bin_data = get_bin_data_for_slab(data, bin_info, slab);
	uint8_t key = bitmap_get_first_logical_byte(slab_data->bitmap,
	    &bin_info->bitmap_info);
	remove_from_bin_data(bin_data, key, slab);
}

static void
bin_data_init(mesh_bin_data_t *bin_data) {
	if (config_stats) {
		memset(&bin_data->stats, 0x0, sizeof(mesh_bin_stats_t));
	}
}

mesh_arena_data_t *
mesh_arena_data_new(tsdn_t *tsdn, base_t *base) {
	size_t size = sizeof(mesh_arena_data_t) +
	    nmeshable_scs * sizeof(mesh_bin_datas_t);
	mesh_arena_data_t *arena_data = (mesh_arena_data_t *)base_alloc(
	    tsdn, base, size, CACHELINE);

	arena_data->bin_datas = (mesh_bin_datas_t *)(arena_data + 1);

	size = sizeof(mesh_bin_data_t) * nmeshable_bins_per_arena;

	mesh_bin_data_t *bin_data_base = (mesh_bin_data_t *)base_alloc(
	    tsdn, base, size, CACHELINE);
	uintptr_t bin_data_addr = (uintptr_t)bin_data_base;

	for (size_t i = 0; i < nmeshable_scs; i++) {
		mesh_bin_datas_t *mesh_bin_datas = &arena_data->bin_datas[i];
		mesh_bin_data_t *addr = (mesh_bin_data_t *)bin_data_addr;
		mesh_bin_datas->bin_data_shards = addr;

		unsigned binind = meshind_to_binind_table[i];
		bin_data_addr += sizeof(mesh_bin_data_t) *
		    bin_infos[binind].n_shards;
	}
	assert(bin_data_addr == (uintptr_t)bin_data_base + size);

	for (size_t i = 0; i < nmeshable_bins_per_arena; i++) {
		bin_data_init(&bin_data_base[i]);
	}
	return arena_data;
}


bool
mesh_boot(void) {
	nmeshable_scs = 0;
	nmeshable_bins_per_arena = 0;
	for (unsigned i = 0; i < SC_NBINS; i++) {
		if (bin_infos[i].nregs <= 8 && bin_infos[i].nregs > 1) {
			meshind_to_binind_table[nmeshable_scs] = i;
			binind_to_meshind_table[i] = nmeshable_scs++;
			nmeshable_bins_per_arena += bin_infos[i].n_shards;
		} else {
			binind_to_meshind_table[i] = SC_NBINS;
		}
	}

	for (unsigned i = nmeshable_scs; i < SC_NBINS; i++) {
		meshind_to_binind_table[nmeshable_scs] = SC_NBINS;
	}

	return nmeshable_scs == 0;
}

/* Stats. */
void
mesh_bin_stats_merge(tsdn_t *tsdn, mesh_bin_stats_t *dst_mesh_bin_stats,
    mesh_arena_data_t *mesh_arena_data, bin_t *bin, szind_t binind,
    unsigned binshard) {
	unsigned meshind = binind_to_meshind_table[binind];
	if (meshind == SC_NBINS) {
		return;
	}
	mesh_bin_datas_t *mesh_bin_datas =
	    &mesh_arena_data->bin_datas[meshind];
	mesh_bin_stats_t *mesh_bin_stats =
	    &mesh_bin_datas->bin_data_shards[binshard].stats;
	for (unsigned i = 0; i < (1 << 8); i++) {
		dst_mesh_bin_stats->shape_counts[i] +=
		    mesh_bin_stats->shape_counts[i];
	}
}
