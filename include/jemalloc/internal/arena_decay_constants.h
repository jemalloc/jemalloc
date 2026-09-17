#ifndef JEMALLOC_INTERNAL_ARENA_DECAY_CONSTANTS_H
#define JEMALLOC_INTERNAL_ARENA_DECAY_CONSTANTS_H

/*
 * Minimal header so both arena.h and tsd_internals.h can share decay-related
 * constants without dragging the full arena types into the tsd parse chain
 * (which is loaded long before arena.h via ckh.h -> tsd.h).
 */

/* Number of event ticks between time checks. */
#define ARENA_DECAY_NTICKS_PER_UPDATE 1000

#endif /* JEMALLOC_INTERNAL_ARENA_DECAY_CONSTANTS_H */
