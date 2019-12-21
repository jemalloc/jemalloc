# `jemalloc` internals

Accurate as of the end of 2019.

## High-level overview

Any given allocation size is internally increased to round it up to the nearest
"size class".  These size classes are chosen to balance chances for reuse,
avoiding overconsumption, and making internal calculations easier.  We divide
the size classes into two types: small (those less than a few pages) and large.
The way in which we allocate depends on whether or not the allocation size is
small or large.  Large allocations are satisfied from a more central page-level
allocator.  For small allocations, we allocate a "slab" from that page-level
allocator, of some small number of pages (1-7, given our current size class
selections and common page sizes).  It is dedicated only to allocations of a
single size class.  (Every slab is a slab of *some* size class; the
distinguishing feature of a slab, as opposed to some other reservation of pages,
is that it is reserved for allocations of a small size class).  The centralized
slab and page allocators are sharded into "arenas".

Sitting in front of the arena-level allocators is a thread-local cache
("tcache").  Allocations up to some particular size are filled from the tcache
when it is nonempty.  When it is empty, the tcache attempts to fill itself using
the more centralized mechanism (using items from a slab if the relevant size is
small, and a page allocation if the size is large).  Deallocated memory is put
into the tcache if possible (and is flushed to the more central allocator if
not).

## Some key concepts

### Size classes

There are two relevant modules here:
- `sc`: Actually sets the size classes (according to static and runtime
    configuration options).
- `sz`: Converts in between various notions of sizes (size in bytes, the size
    class index, or a "page size" index, which is used in making requests from
    the page-level allocator).

### Extents

An extent is a page-aligned, multiple-of-page-sized region of the virtual
address space owned by jemalloc.

### Extent states and purging

Modern architectures perform virtual -> physical address translation, and modern
OSs in turn allow features like overcommit; reserving virtual address space
without backing it with physical pages.

We say that a page of virtual memory is in one of four states:
- Active: In use by the application
- Dirty: No longer in use, but we haven't told the OS yet.
- Muzzy: We've told the OS to reclaim the page, but asynchronously.  It may
  never get reclaimed, and definitely won't if we touch it again.
- Retained: We've told the OS to reclaim the page, and waited synchronously for
  it to do so.  It will be zeroed upon next access.

(Sometimes the latter three states are grouped together as "inactive").

The process of taking a page from dirty -> muzzy -> retained is "purging".
Earlier stages of purging consume more memory, but are less expensive (both in
the sense that it's easier to put a page into that state, and that it's more
CPU-efficient to reuse a page in an earlier state; there's less of a chance of
soft faults).

We don't always use the retained page state; sometimes we just unmap the extent.
See the section on the page-level allocator below, for details.

### Base allocators, bootstrapping

The `base` module defines the lowest-level of allocation support.  It supports
allocation-only (no freeing), and can be replaced with "extent hooks" that
replace the OS-level page manipulation facilities.  Each arena is associated
with a single base allocator.  Each `base_t` has a unique index, and each arena
is associated with a single base allocator (whose index is the user-visible
index for the arena).

Base 0 and arena 0 are used for internal metadata allocations.  They're
sometimes called "b0" and "a0" for short.

### Thread-specific data

The `tsd_t` struct contains all the per-thread data used by any module.  We
abstract this out into one place for two reasons:
1. So that it's easy for us to control the  layout
2. Touching TLS may allocate in some situations; we want to unify bootstrapping
   logic.

## Global metadata

### Edata

The edata ("extent data") module describes a particular extent.  It includes
the extent state, the index of the owning arena, whether or not the described
extent is a slab (if active), and other useful hint information.  If the extent
is a slab, the associated edata keeps a bitmap indicating which items in the
slab are free or in use.

The edata struct contains various internal linkages, that let it be put into
pairing heaps and lists.

### Rtree

The rtree is a 2- or 3- level radix tree that maps an extent address to the
edata describing it.  To allow thread-local caching to avoid having to chase the
pointer to the edata to find the size of an allocation, we also track the size
class and index within the rtree.  New levels of the rtree are lazily filled in
with memory allocated from b0.

Walking this radix tree can be expensive; we therefore use thread-local caching
of lookup results in intermediate levels to avoid having to.

### Profiling information

TODO.

## The arena

Above, we described the arena as a sharding mechanism for centralized extent
allocation. Historically, it was the *sole* unit for reducing mutex contention.
`N` arenas meant `N` more mutexes contention was divided across.  We are slowly
working to break the centrality of this abstraction (and allow more granular and
independent sharding of different centralized abstractions), but this is very
much in progress.

By default, threads are tied to some specific arena at the time of their first
allocation.  A thread's arena is where it will look for more memory for a
particular allocation request, after the tcache.

Each arena contains several important bits of state:
- For each small size class, some number of bins.  The bins keep track of slabs
  that are not currently full.  We allow multiple bins for a single size class
  as a contention-reduction mechanism.
- Stats information
- Profiling information
- A page level allocator
- Purging metadata

### Bins

A bin tracks slabs of some size class.  Arena-level requests for small
allocations try to get it from a non-full slab first, allocating a new slab from
the page-level allocator only if it doesn't find any.

### The page-level allocator

Within an arena, there are three `ecache_t` ("extent cache") instances, one for
each inactive purging state (i.e.  `ecache_dirty`, `ecache_muzzy`, and
`ecache_retained`).  A request for an extent tries to reuse one by calling
`ecache_alloc` once on the dirty and then once on the muzzy ecache.  If this
fails, it calls `ecache_alloc_grow` on the retained `ecache`.  The behavior then
depends on `opt_retain`.  If retain is off, then the ecache is empty and
`ecache_alloc_grow` simply calls whatever `alloc` hook is active (which boils
down to `mmap` most of the time).  If retain is on, though, we never deallocate
virtual address space; instead, we go down the `extent_grow_retained` pathway,
which allocates virtual address space in exponentially-growing chunks.

Retained is ugly and bifurcates behavior.  So why have it?  Two reasons:
- The Linux memory manager fragments badly, and limits the maximum number of
  mappings it will allow.  With large heaps with non-uniform lifetimes, this
  forces tradeoffs between exhausting the heap and leaving around memory that we
  don't plan on using.
- On Windows, mappings from different `VirtualAlloc` calls don't implicitly
  merge, and you're not allowed to purge across them.  Having large regions of
  virtual address space that allocations come from lets us reuse memory across
  different sizes of allocations without the allocations having to be close in
  size.

The ecache functions and the associated functionality live in the `extent`
module.  This manages locking, metadata management, and growth policies.  An
`ecache_t` wraps an `eset_t`, which tracks extents all in the same state, and
does first-fit extent selection and picks regions to purge.  These functions
also require additional `edata_t` objects to describe extents following a split.
It obtains them from the arena-level `edata_cache_t`, which allows `edata_t`
reuse (it gets them from the arena's corresponding base; recall that bases don't
support deallocation).

### Purging
TODO

## Tcache
TODO

### Thread event module
TODO

### Tcache GC
TODO

## Non-core functionality

### `mallctl` interface
TODO

### Background threads
TODO

### Reentrancy handling
We support interposing the system `malloc` on at least a few platforms (glibc
Linux being the big one).  But since we're not tightly coupled with libc, there
can be scenarios where code calls jemalloc, which in turn calls a libc function,
which in turn allocates.  To try to minimize the chances of an error in such
situations, we keep a reentrancy-level counter in the TSD, which takes us down
slow paths which try to minimize dependencies (doing things like allocating out
of a0, not sampling any allocations for profiling, etc.).  As a hack to allow
close personal friends of the jemalloc team to allocate from within extent
hooks, we also turn on these reentrancy guards around extent hook calls.
Eventually, we're going to have think about what sorts of allocator activity is
allowed within an extent hook, and commit to something more concrete.  Until
then, if you're not a close personal friend of the jemalloc team, don't rely on
this behavior, which we'll gladly break at will.

To test this behavior, the `test_hooks` module wraps some core libc
functionality.  The testing framework runs most tests multiple times, with a
hook function executed before the call that does some allocation activity.

### Safety checking
TODO

### Per-cpu arenas
TODO

### Utilities
Fast dynamic division, prng facilities, the emitter, hooks, mutex profiling and
witnesses, data structures (pairing heap, hash map, rb tree, linked lists),
seqlocks, spin, bit manipulation, smoothstep, atomics.

TODO

## Build system

TODO
