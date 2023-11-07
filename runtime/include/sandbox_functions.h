#pragma once

#include <sys/mman.h>
#include <stddef.h>
#include <stdint.h>

#include "panic.h"
#include "tenant.h"
#include "sandbox_types.h"

/***************************
 * Public API              *
 **************************/

extern bool runtime_exponential_service_time_simulation_enabled;
extern struct perf_window * worker_perf_windows[1024];
struct sandbox *sandbox_alloc(struct module *module, struct http_session *session, struct route *route,
                              struct tenant *tenant, uint64_t admissions_estimate, void *req_handle,
			      uint8_t rpc_id);
int             sandbox_prepare_execution_environment(struct sandbox *sandbox);
uint64_t        sandbox_free(struct sandbox *sandbox, uint64_t *ret);
void            sandbox_main(struct sandbox *sandbox);
void            sandbox_switch_to(struct sandbox *next_sandbox);
void 		sandbox_send_response(struct sandbox *sandbox, uint8_t response_code);

/**
 * Free Linear Memory, leaving stack in place
 * @param sandbox
 */
static inline void
sandbox_free_linear_memory(struct sandbox *sandbox)
{
	assert(sandbox != NULL);
	assert(sandbox->memory != NULL);
	module_free_linear_memory(sandbox->module, (struct wasm_memory *)sandbox->memory);
	sandbox->memory = NULL;
}

/**
 * Given a sandbox, returns the module that sandbox is executing
 * @param sandbox the sandbox whose module we want
 * @return the module of the provided sandbox
 */
static inline struct module *
sandbox_get_module(struct sandbox *sandbox)
{
	if (!sandbox) return NULL;
	return sandbox->module;
}

static inline uint64_t
sandbox_get_priority(void *element)
{
	assert(element != NULL);
	struct sandbox *sandbox = (struct sandbox *)element;
	return sandbox->absolute_deadline;
}

static inline uint64_t
sandbox_get_execution_cost(void *element, int thread_id) 
{
	assert(element != NULL);
	struct sandbox *sandbox = (struct sandbox *)element;
	if (runtime_exponential_service_time_simulation_enabled) {
		assert(sandbox->estimated_cost != 0);
		return sandbox->estimated_cost;
	} else {
		uint32_t uid = sandbox->route->admissions_info.uid;
		return perf_window_get_percentile(&worker_perf_windows[thread_id][uid],
                                          	sandbox->route->admissions_info.percentile,
                                          	sandbox->route->admissions_info.control_index);

	}
}

static inline void
sandbox_process_scheduler_updates(struct sandbox *sandbox)
{
	if (tenant_is_paid(sandbox->tenant)) {
		atomic_fetch_sub(&sandbox->tenant->remaining_budget, sandbox->last_state_duration);
	}
}
