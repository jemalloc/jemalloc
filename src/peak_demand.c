#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/peak_demand.h"

void
peak_demand_init(peak_demand_t *peak_demand, uint64_t interval_ms) {
	assert(interval_ms > 0);
	peak_demand->epoch = 0;
	uint64_t interval_ns = interval_ms * 1000 * 1000;
	peak_demand->epoch_interval_ns = interval_ns / PEAK_DEMAND_NBUCKETS;
	memset(peak_demand->nactive_max, 0, sizeof(peak_demand->nactive_max));
}

static uint64_t
peak_demand_epoch_ind(peak_demand_t *peak_demand) {
	return peak_demand->epoch % PEAK_DEMAND_NBUCKETS;
}

static nstime_t
peak_demand_next_epoch_advance(peak_demand_t *peak_demand) {
	uint64_t epoch = peak_demand->epoch;
	uint64_t ns = (epoch + 1) * peak_demand->epoch_interval_ns;
	nstime_t next;
	nstime_init(&next, ns);
	return next;
}

static uint64_t
peak_demand_maybe_advance_epoch(peak_demand_t *peak_demand,
    const nstime_t *now) {
	nstime_t next_epoch_advance =
	    peak_demand_next_epoch_advance(peak_demand);
	if (nstime_compare(now, &next_epoch_advance) < 0) {
		return peak_demand_epoch_ind(peak_demand);
	}
	uint64_t next_epoch = nstime_ns(now) / peak_demand->epoch_interval_ns;
	assert(next_epoch > peak_demand->epoch);
	/*
	 * If we missed more epochs, than capacity of circular buffer
	 * (PEAK_DEMAND_NBUCKETS), re-write no more than PEAK_DEMAND_NBUCKETS
	 * items as we don't want to zero out same item multiple times.
	 */
	if (peak_demand->epoch + PEAK_DEMAND_NBUCKETS < next_epoch) {
		peak_demand->epoch = next_epoch - PEAK_DEMAND_NBUCKETS;
	}
	while (peak_demand->epoch < next_epoch) {
		++peak_demand->epoch;
		uint64_t ind = peak_demand_epoch_ind(peak_demand);
		peak_demand->nactive_max[ind] = 0;
	}
	return peak_demand_epoch_ind(peak_demand);
}

void
peak_demand_update(peak_demand_t *peak_demand, const nstime_t *now,
    size_t nactive) {
	uint64_t ind = peak_demand_maybe_advance_epoch(peak_demand, now);
	size_t *epoch_nactive = &peak_demand->nactive_max[ind];
	if (nactive > *epoch_nactive) {
		*epoch_nactive = nactive;
	}
}

size_t
peak_demand_nactive_max(peak_demand_t *peak_demand) {
	size_t nactive_max = peak_demand->nactive_max[0];
	for (int i = 1; i < PEAK_DEMAND_NBUCKETS; ++i) {
		if (peak_demand->nactive_max[i] > nactive_max) {
			nactive_max = peak_demand->nactive_max[i];
		}
	}
	return nactive_max;
}
