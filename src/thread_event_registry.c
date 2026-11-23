#include "jemalloc/internal/jemalloc_preamble.h"

#include "jemalloc/internal/jemalloc_internal_inlines_a.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/peak_event.h"
#include "jemalloc/internal/prof.h"
#include "jemalloc/internal/stats.h"
#include "jemalloc/internal/tcache.h"
#include "jemalloc/internal/thread_event.h"
#include "jemalloc/internal/thread_event_registry.h"
#include "jemalloc/internal/tsd.h"
#include "jemalloc/internal/witness.h"

static malloc_mutex_t uevents_mu;

#ifdef JEMALLOC_DEBUG_FAST
/*
 * The dalloc ([0]) and alloc ([1]) enabled masks, with TE_ENABLED_MASK_VALID
 * set. The built-in handlers' enabled() only read options that are final by
 * experimental_thread_events_boot(), and the user handlers' only depend on
 * which slots are registered, so the masks are computed at boot and again
 * whenever a user handler is registered. Before boot they are 0 and
 * te_enabled_mask() asks the handlers directly.
 */
#	define TE_ENABLED_MASK_VALID (1U << 31)
static atomic_u32_t te_enabled_masks[2];

static uint32_t
te_enabled_mask_compute(bool is_alloc) {
	te_base_cb_t **handlers = is_alloc ? te_alloc_handlers
	                                   : te_dalloc_handlers;
	int      count = is_alloc ? te_alloc_count : te_dalloc_count;
	uint32_t mask = TE_ENABLED_MASK_VALID;
	assert(count < 31);
	for (int i = 0; i < count; i++) {
		if (te_enabled_yes == handlers[i]->enabled()) {
			mask |= 1U << i;
		}
	}
	return mask;
}

static void
te_enabled_masks_update(void) {
	atomic_store_u32(
	    &te_enabled_masks[0], te_enabled_mask_compute(false), ATOMIC_RELEASE);
	atomic_store_u32(
	    &te_enabled_masks[1], te_enabled_mask_compute(true), ATOMIC_RELEASE);
}

uint32_t
te_enabled_mask(bool is_alloc) {
	uint32_t mask = atomic_load_u32(
	    &te_enabled_masks[is_alloc], ATOMIC_ACQUIRE);
	if (unlikely(mask == 0)) {
		mask = te_enabled_mask_compute(is_alloc);
	}
	return mask & ~TE_ENABLED_MASK_VALID;
}
#endif

bool
experimental_thread_events_boot(void) {
#ifdef JEMALLOC_DEBUG_FAST
	te_enabled_masks_update();
#endif
	return malloc_mutex_init(&uevents_mu, "thread_events",
	    WITNESS_RANK_THREAD_EVENTS_USER, malloc_mutex_rank_exclusive);
}

#define TE_REGISTER_ERRCODE_FULL_SLOTS -1
#define TE_REGISTER_ERRCODE_ALREADY_REGISTERED -2

static user_hook_object_t uevents_storage[TE_MAX_USER_EVENTS] = {
    {NULL, 0, false},
};

static atomic_p_t uevent_obj_p[TE_MAX_USER_EVENTS] = {
    ATOMIC_INIT(0),
};

static inline bool
user_object_eq(user_hook_object_t *lhs, user_hook_object_t *rhs) {
	assert(lhs != NULL && rhs != NULL);

	return lhs->callback == rhs->callback && lhs->interval == rhs->interval
	    && lhs->is_alloc_only == rhs->is_alloc_only;
}

/*
 * Return slot number that event is registered at on success
 *     it will be [0, TE_MAX_USER_EVENTS)
 * Return negative value on some error
 */
static inline int
te_register_user_handler_locked(user_hook_object_t *new_obj) {
	/* Attempt to find the free slot in global register */
	for (int i = 0; i < TE_MAX_USER_EVENTS; ++i) {
		user_hook_object_t *p = (user_hook_object_t *)atomic_load_p(
		    &uevent_obj_p[i], ATOMIC_ACQUIRE);

		if (p && user_object_eq(p, new_obj)) {
			/* Same callback and interval are registered - no error. */
			return TE_REGISTER_ERRCODE_ALREADY_REGISTERED;
		} else if (p == NULL) {
			/* Empty slot */
			uevents_storage[i] = *new_obj;
			atomic_fence(ATOMIC_SEQ_CST);
			atomic_store_p(&uevent_obj_p[i], &uevents_storage[i],
			    ATOMIC_RELEASE);
#ifdef JEMALLOC_DEBUG_FAST
			te_enabled_masks_update();
#endif
			return i;
		}
	}

	return TE_REGISTER_ERRCODE_FULL_SLOTS;
}

static inline user_hook_object_t *
uobj_get(size_t cb_idx) {
	assert(cb_idx < TE_MAX_USER_EVENTS);
	return (user_hook_object_t *)atomic_load_p(
	    &uevent_obj_p[cb_idx], ATOMIC_ACQUIRE);
}

te_enabled_t
te_user_event_enabled(size_t ue_idx, bool is_alloc) {
	assert(ue_idx < TE_MAX_USER_EVENTS);
	user_hook_object_t *obj = uobj_get(ue_idx);
	if (!obj) {
		return te_enabled_not_installed;
	}
	if (is_alloc || !obj->is_alloc_only) {
		return te_enabled_yes;
	}
	return te_enabled_no;
}

static inline uint64_t
new_event_wait(size_t cb_idx) {
	user_hook_object_t *obj = uobj_get(cb_idx);
	/* Enabled should have guarded it */
	assert(obj);
	return obj->interval;
}

static uint64_t
postponed_event_wait(tsd_t *tsd) {
	return TE_MIN_START_WAIT;
}

static inline void
handler_wrapper(tsd_t *tsd, bool is_alloc, size_t cb_idx) {
	user_hook_object_t *obj = uobj_get(cb_idx);
	/* Enabled should have guarded it */
	assert(obj);
	uint64_t alloc = tsd_thread_allocated_get(tsd);
	uint64_t dalloc = tsd_thread_deallocated_get(tsd);

	pre_reentrancy(tsd, NULL);
	obj->callback(is_alloc, alloc, dalloc);
	post_reentrancy(tsd);
}

#define TE_USER_HANDLER_BINDING_IDX(i)                                         \
	static te_enabled_t te_user_alloc_enabled##i(void) {                   \
		return te_user_event_enabled(i, true);                         \
	}                                                                      \
	static te_enabled_t te_user_dalloc_enabled##i(void) {                  \
		return te_user_event_enabled(i, false);                        \
	}                                                                      \
	static uint64_t te_user_new_event_wait_##i(tsd_t *tsd) {               \
		return new_event_wait(i);                                      \
	}                                                                      \
	static void te_user_alloc_handler_call##i(tsd_t *tsd) {                \
		handler_wrapper(tsd, true, i);                                 \
	}                                                                      \
	static void te_user_dalloc_handler_call##i(tsd_t *tsd) {               \
		handler_wrapper(tsd, false, i);                                \
	}                                                                      \
	static te_base_cb_t user_alloc_handler##i = {                          \
	    .enabled = &te_user_alloc_enabled##i,                              \
	    .new_event_wait = &te_user_new_event_wait_##i,                     \
	    .postponed_event_wait = &postponed_event_wait,                     \
	    .event_handler = &te_user_alloc_handler_call##i};                  \
	static te_base_cb_t user_dalloc_handler##i = {                         \
	    .enabled = &te_user_dalloc_enabled##i,                             \
	    .new_event_wait = &te_user_new_event_wait_##i,                     \
	    .postponed_event_wait = &postponed_event_wait,                     \
	    .event_handler = &te_user_dalloc_handler_call##i}

TE_USER_HANDLER_BINDING_IDX(0);
TE_USER_HANDLER_BINDING_IDX(1);
TE_USER_HANDLER_BINDING_IDX(2);
TE_USER_HANDLER_BINDING_IDX(3);

/* Table of all the thread events. */
te_base_cb_t *te_alloc_handlers[te_alloc_count] = {
#ifdef JEMALLOC_PROF
    &prof_sample_te_handler,
#endif
    &stats_interval_te_handler, &tcache_gc_te_handler,
#ifdef JEMALLOC_STATS
    &peak_te_handler,
#endif
    &user_alloc_handler0, &user_alloc_handler1, &user_alloc_handler2,
    &user_alloc_handler3};

te_base_cb_t *te_dalloc_handlers[te_dalloc_count] = {&tcache_gc_te_handler,
#ifdef JEMALLOC_STATS
    &peak_te_handler,
#endif
    &user_dalloc_handler0, &user_dalloc_handler1, &user_dalloc_handler2,
    &user_dalloc_handler3};

static inline bool
te_update_tsd(tsd_t *tsd, uint64_t new_wait, size_t ue_idx, bool is_alloc) {
	bool     needs_recompute = false;
	te_ctx_t ctx;
	uint64_t next, current, cur_wait;

	if (is_alloc) {
		tsd_te_datap_get_unsafe(tsd)
		    ->alloc_wait[te_alloc_user0 + ue_idx] = new_wait;
	} else {
		tsd_te_datap_get_unsafe(tsd)
		    ->dalloc_wait[te_dalloc_user0 + ue_idx] = new_wait;
	}
	te_ctx_get(tsd, &ctx, is_alloc);

	next = te_ctx_next_event_get(&ctx);
	current = te_ctx_current_bytes_get(&ctx);
	cur_wait = next - current;

	if (new_wait < cur_wait) {
		/*
		 * Set last event to current (same as when te inits).  This
		 * will make sure that all the invariants are correct, before
		 * we adjust next_event and next_event fast.
		 */
		te_ctx_last_event_set(&ctx, te_ctx_current_bytes_get(&ctx));
		te_adjust_thresholds_helper(tsd, &ctx, new_wait);
		needs_recompute = true;
	}
	return needs_recompute;
}

static inline void
te_recalculate_current_thread_data(tsdn_t *tsdn, int ue_idx, bool alloc_only) {
	bool recompute = false;
	/* we do not need lock to recalculate the events on the current thread */
	assert(ue_idx < TE_MAX_USER_EVENTS);
	tsd_t *tsd = tsdn_null(tsdn) ? tsd_fetch() : tsdn_tsd(tsdn);
	if (tsd) {
		uint64_t new_wait = new_event_wait(ue_idx);
		recompute = te_update_tsd(tsd, new_wait, ue_idx, true);
		if (!alloc_only) {
			recompute = te_update_tsd(tsd, new_wait, ue_idx, false)
			    || recompute;
		}

		if (recompute) {
			te_recompute_fast_threshold(tsd);
		}
	}
}

int
te_register_user_handler(tsdn_t *tsdn, user_hook_object_t *te_uobj) {
	int ret;
	int reg_retcode;
	if (!te_uobj || !te_uobj->callback || te_uobj->interval == 0) {
		return EINVAL;
	}

	malloc_mutex_lock(tsdn, &uevents_mu);
	reg_retcode = te_register_user_handler_locked(te_uobj);
	malloc_mutex_unlock(tsdn, &uevents_mu);

	if (reg_retcode >= 0) {
		te_recalculate_current_thread_data(
		    tsdn, reg_retcode, te_uobj->is_alloc_only);
		ret = 0;
	} else if (reg_retcode == TE_REGISTER_ERRCODE_ALREADY_REGISTERED) {
		ret = 0;
	} else {
		ret = EINVAL;
	}

	return ret;
}
