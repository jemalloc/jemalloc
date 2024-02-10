#!/bin/sh

# This value of max_batched_size effectively requires all bins to be batched;
# our page limits are fuzzy, but we bound slab item counts to 2**32, so we'd be
# at multi-gigabyte minimum page sizes.
# The reason for this sort of hacky approach is that we want to
# allocate/deallocate PAGE/2-sized objects (to trigger the "non-empty" ->
# "empty" and "non-empty"-> "full" transitions often, which have special
# handling). But the value of PAGE isn't easily available in test scripts.
export MALLOC_CONF="narenas:2,bin_shards:1-1000000000:3,max_batched_size:1000000000,remote_free_max_batch:1,remote_free_max:4"
