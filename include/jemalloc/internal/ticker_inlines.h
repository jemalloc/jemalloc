#ifndef JEMALLOC_INTERNAL_TICKER_INLINES_H
#define JEMALLOC_INTERNAL_TICKER_INLINES_H

static inline void
ticker_init(ticker_t *ticker, int32_t nticks) {
	ticker->tick = nticks;
	ticker->nticks = nticks;
}

static inline void
ticker_copy(ticker_t *ticker, const ticker_t *other) {
	*ticker = *other;
}

static inline int32_t
ticker_read(const ticker_t *ticker) {
	return ticker->tick;
}

static inline bool
ticker_ticks(ticker_t *ticker, int32_t nticks) {
	if (unlikely(ticker->tick < nticks)) {
		ticker->tick = ticker->nticks;
		return true;
	}
	ticker->tick -= nticks;
	return(false);
}

static inline bool
ticker_tick(ticker_t *ticker) {
	return ticker_ticks(ticker, 1);
}

#endif /* JEMALLOC_INTERNAL_TICKER_INLINES_H */
