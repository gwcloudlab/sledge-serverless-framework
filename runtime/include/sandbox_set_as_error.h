#pragma once

#include <assert.h>
#include <stdint.h>

#include "admissions_control.h"
#include "traffic_control.h"
#include "arch/getcycles.h"
#include "local_completion_queue.h"
#include "local_runqueue.h"
#include "sandbox_state.h"
#include "sandbox_functions.h"
#include "sandbox_perf_log.h"
#include "sandbox_state_history.h"
#include "sandbox_summarize_page_allocations.h"
#include "panic.h"

/**
 * Transitions a sandbox to the SANDBOX_ERROR state.
 * This can occur during initialization or execution
 * Unmaps linear memory, removes from the runqueue (if on it), and adds to the completion queue
 * Because the stack is still in use, freeing the stack is deferred until later
 *
 * @param sandbox the sandbox erroring out
 * @param last_state the state the sandbox is transitioning from. This is expressed as a constant to
 * enable the compiler to perform constant propagation optimizations.
 */
static inline void
sandbox_set_as_error(struct sandbox *sandbox, sandbox_state_t last_state)
{
	assert(sandbox);
	sandbox->state = SANDBOX_ERROR;
	uint64_t now   = __getcycles();

	switch (last_state) {
	case SANDBOX_ALLOCATED:
		break;
	case SANDBOX_INITIALIZED:
		/* This is a global work-shedding scenario, where we kill a job from the global queue */
		break;
	case SANDBOX_RUNNABLE:
	case SANDBOX_RUNNING_SYS:
	case SANDBOX_INTERRUPTED:
	case SANDBOX_PREEMPTED:
		if (sandbox->owned_worker_idx >= 0) { local_runqueue_delete(sandbox); }
	case SANDBOX_ASLEEP:
		sandbox_free_linear_memory(sandbox);
		sandbox_deinit_http_buffers(sandbox);
		break;
	default:
		panic("Sandbox %lu | Illegal transition from %s to Error\n", sandbox->id,
		      sandbox_state_stringify(last_state));
	}

	/* State Change Bookkeeping */
	assert(now > sandbox->timestamp_of.last_state_change);
	sandbox->last_state_duration = now - sandbox->timestamp_of.last_state_change;
	sandbox->duration_of_state[last_state] += sandbox->last_state_duration;
	sandbox->timestamp_of.last_state_change = now;
	sandbox->total_time                     = now - sandbox->timestamp_of.request_arrival;
	sandbox_state_history_append(&sandbox->state_history, SANDBOX_ERROR);
	sandbox_state_totals_increment(SANDBOX_ERROR);
	sandbox_state_totals_decrement(last_state);

	/* Admissions Control Post Processing */
	admissions_control_subtract(sandbox->admissions_estimate);

	/* Terminal State Logging */
	sandbox_perf_log_print_entry(sandbox);
	sandbox_summarize_page_allocations(sandbox);

	/* Does not add to completion queue until in cooperative scheduler */
}

static inline void
sandbox_exit_error(struct sandbox *sandbox)
{
	// assert(sandbox->state == SANDBOX_RUNNING_SYS || sandbox->state == SANDBOX_INTERRUPTED);
	sandbox_set_as_error(sandbox, sandbox->state);

	sandbox_process_scheduler_updates(sandbox);
}
