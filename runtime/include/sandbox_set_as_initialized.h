#pragma once

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "arch/context.h"
#include "current_sandbox.h"
#include "ps_list.h"
#include "sandbox_request.h"
#include "sandbox_state_history.h"
#include "sandbox_types.h"

/**
 * Transitions a sandbox to the SANDBOX_INITIALIZED state.
 * The sandbox was already zeroed out during allocation
 * @param sandbox an uninitialized sandbox
 * @param sandbox_request the request we are initializing the sandbox from
 * @param allocation_timestamp timestamp of allocation
 */
static inline void
sandbox_set_as_initialized(struct sandbox *sandbox, struct sandbox_request *sandbox_request,
                           uint64_t allocation_timestamp)
{
	assert(sandbox);
	assert(sandbox->state == SANDBOX_ALLOCATED);
	assert(sandbox_request != NULL);
	assert(allocation_timestamp > 0);
	sandbox->state = SANDBOX_INITIALIZED;
	uint64_t now   = __getcycles();

	/* Copy State from Sandbox Request */
	sandbox->id                           = sandbox_request->id;
	sandbox->absolute_deadline            = sandbox_request->absolute_deadline;
	sandbox->admissions_estimate          = sandbox_request->admissions_estimate;
	sandbox->client_socket_descriptor     = sandbox_request->socket_descriptor;
	sandbox->timestamp_of.request_arrival = sandbox_request->request_arrival_timestamp;
	/* Copy the socket descriptor and address of the client invocation */
	memcpy(&sandbox->client_address, &sandbox_request->socket_address, sizeof(struct sockaddr));


	/* Initialize the sandbox's context, stack, and instruction pointer */
	/* stack.start points to the bottom of the usable stack, so add stack_size to get to top */
	arch_context_init(&sandbox->ctxt, (reg_t)current_sandbox_start,
	                  (reg_t)sandbox->stack.start + sandbox->stack.size);

	/* Initialize Parsec control structures */
	ps_list_init_d(sandbox);


	/* State Change Bookkeeping */
	sandbox->duration_of_state[SANDBOX_ALLOCATED] = now - allocation_timestamp;
	sandbox->timestamp_of.allocation              = allocation_timestamp;
	sandbox->timestamp_of.last_state_change       = allocation_timestamp;
	sandbox_state_history_append(sandbox, SANDBOX_INITIALIZED);
	runtime_sandbox_total_increment(SANDBOX_INITIALIZED);
}
