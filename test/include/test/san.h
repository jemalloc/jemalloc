static inline bool
extent_is_guarded(tsdn_t *tsdn, void *ptr) {
	edata_t *edata = emap_edata_lookup(tsdn, &arena_emap_global, ptr);
	return edata_guarded_get(edata);
}

