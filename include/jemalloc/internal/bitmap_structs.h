#ifndef JEMALLOC_INTERNAL_BITMAP_STRUCTS_H
#define JEMALLOC_INTERNAL_BITMAP_STRUCTS_H

struct bitmap_level_s {
	/* Offset of this level's groups within the array of groups. */
	size_t group_offset;
};

struct bitmap_info_s {
	/* Logical number of bits in bitmap (stored at bottom level). */
	size_t nbits;

	/* Number of groups necessary for nbits. */
	size_t ngroups;
};

#endif /* JEMALLOC_INTERNAL_BITMAP_STRUCTS_H */
