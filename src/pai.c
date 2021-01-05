#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"


void
pai_dalloc_batch_default(tsdn_t *tsdn, pai_t *self,
    edata_list_active_t *list) {
	edata_t *edata;
	while ((edata = edata_list_active_first(list)) != NULL) {
		edata_list_active_remove(list, edata);
		pai_dalloc(tsdn, self, edata);
	}
}
