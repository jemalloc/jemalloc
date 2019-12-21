# Terminology

This is a quick-reference companion to INTERNALS.md:
- `a0`, `b0`: The arena and base allocator used for bootstrapping and global
    metadata operations
- `bin`: Contains slabs, all with the same size class.  Does first-fit among
    slabs, and tracks both full and non-full ones (in separate heaps).  Scoped
    to an arena.
- `edata_t`: The metadata structure that describes an extent (its state,
    beginning and end addresses, size, etc.).  "Extent data".
- Extent: A region of virtual memory.
- `ecache`: The struct (and level of the allocator) that handles page-level
    allocation for a particular state.  Contains an eset.  "Extent cache".
- `edata_cache_t`: A store of unused `edata_t` objects, which can be used as a
    source of them when splitting or coalescing extents.  "Cache of edatas".
- `edata_heap_t`: A pairing heap of `edata_t` objects, ordered by first-fit
    (first by allocation serial number, then by address).  "Heap of edatas."
- `eset_t`: A collection of inactive extents, and facilities to pick one to
    satisfy an allocation request.  "Extent set".
- Large size class: A size class that's big enough to come directly from the
    page-level allocator, without being in a bin.
- `rtree`: The radix tree that maps virtual addresses to the `edata_t` objects
    describing them.
- Slab: An extent dedicate to storing small objects, laid out contiguously.
- Small size class: A size class that's stored in a slab.
- Size class: The size we round up an allocation request to.  Represented with
    an `szind_t`.
