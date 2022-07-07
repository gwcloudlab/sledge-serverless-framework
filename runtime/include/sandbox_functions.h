#pragma once

#include <sys/mman.h>
#include <stddef.h>
#include <stdint.h>

#include "client_socket.h"
#include "panic.h"
#include "sandbox_types.h"
#include "current_sandbox.h"

/***************************
 * Public API              *
 **************************/

struct sandbox *sandbox_alloc(struct module *module, int socket_descriptor, const struct sockaddr *socket_address,
                              uint64_t request_arrival_timestamp, uint64_t admissions_estimate);
int             sandbox_prepare_execution_environment(struct sandbox *sandbox);
void            sandbox_free(struct sandbox *sandbox);
void            sandbox_main(struct sandbox *sandbox);
void            sandbox_switch_to(struct sandbox *next_sandbox);
static inline void
sandbox_close_http(struct sandbox *sandbox)
{
	assert(sandbox != NULL);

	int rc = epoll_ctl(worker_thread_epoll_file_descriptor, EPOLL_CTL_DEL, sandbox->client_socket_descriptor, NULL);
	if (unlikely(rc < 0)) panic_err();

	client_socket_close(sandbox->client_socket_descriptor, &sandbox->client_address);
}

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
 * Deinitialize Linear Memory, cleaning up the backing buffer
 * @param sandbox
 */
static inline void
sandbox_deinit_http_buffers(struct sandbox *sandbox)
{
	assert(sandbox);
	vec_u8_deinit(&sandbox->request);
	vec_u8_deinit(&sandbox->response);
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
	struct sandbox *sandbox = (struct sandbox *)element;
	return sandbox->absolute_deadline;
};

static inline void
sandbox_update_pq_idx_in_runqueue(void *element, size_t idx)
{
	// if (!element) return;
	struct sandbox *sandbox     = (struct sandbox *)element;
	sandbox->pq_idx_in_runqueue = idx;
}

static inline void
global_sandbox_update_pq_idx_in_module_queue(void *element, size_t idx)
{
	// if (!element) return;
	struct sandbox *sandbox         = (struct sandbox *)element;
	sandbox->pq_idx_in_module_queue = idx;
}

static inline void
local_sandbox_meta_update_pq_idx_in_module_queue(void *element, size_t idx)
{
	// if (!element) return;
	struct sandbox_metadata *sandbox_meta = (struct sandbox_metadata *)element;
	sandbox_meta->pq_idx_in_module_queue    = idx;
}

static inline void
sandbox_open_http(struct sandbox *sandbox)
{
	assert(sandbox != NULL);

	http_parser_init(&sandbox->http_parser, HTTP_REQUEST);

	/* Set the sandbox as the data the http-parser has access to */
	sandbox->http_parser.data = sandbox;

	/* Freshly allocated sandbox going runnable for first time, so register client socket with epoll */
	// struct epoll_event accept_evt;
	// accept_evt.data.ptr = (void *)sandbox;
	// accept_evt.events   = EPOLLIN | EPOLLOUT | EPOLLET;
	// int rc = epoll_ctl(worker_thread_epoll_file_descriptor, EPOLL_CTL_ADD, sandbox->client_socket_descriptor,
	//                    &accept_evt);
	// if (unlikely(rc < 0)) panic_err();
}

// #include <fcntl.h>
static inline void
sandbox_process_scheduler_updates(struct sandbox *sandbox)
{
	if (module_is_paid(sandbox->module)) {
		atomic_fetch_sub(&sandbox->module->remaining_budget, sandbox->last_state_duration);
	}

#ifdef TRAFFIC_CONTROL
	// if (fcntl(sandbox->client_socket_descriptor, F_GETFD) == -1 && sandbox->state != SANDBOX_ERROR &&
	// sandbox->state!=SANDBOX_RETURNED) { 	printf("Killed sandbox says bye id=%lu, rem=%ld, state=%u!\n",
	// sandbox->id, 	       sandbox->remaining_execution, sandbox->state);
	// }
	// sandbox->remaining_execution -= sandbox->last_state_duration;
	// debuglog("----------\nsandbox remaining: %ld\n----------\n", sandbox->remaining_execution);
	// if(sandbox->remaining_execution < 0) panic ("Sandbox OVER expected!");

	struct comm_with_worker *cfw = &comm_from_workers[worker_thread_idx];
	// struct comm_with_worker *cfw_extra = &comm_from_workers_extra[worker_thread_idx];

	assert(cfw);
	// assert(cfw_extra);

	struct message new_message;

	new_message.module                        = sandbox->module;
	new_message.sandbox                       = sandbox;
	new_message.sandbox_id                    = sandbox->id;
	new_message.sandbox_meta                  = sandbox->sandbox_meta;
	new_message.absolute_deadline             = sandbox->absolute_deadline;
	new_message.state                         = sandbox->state;
	new_message.sender_worker_idx             = worker_thread_idx;
	new_message.exceeded_estimation           = sandbox->exceeded_estimation;
	new_message.last_extra_demand_timestamp   = sandbox->timestamp_of.last_extra_demand_request;
	new_message.extra_demand_request_approved = false;

	new_message.prev_rem_exec = sandbox->remaining_execution;
	new_message.last_exec_dur = sandbox->last_state_duration;
	new_message.reserv        = sandbox->module->reservation_percentile;

	assert(sandbox->remaining_execution >= 0);

	const uint64_t now = __getcycles();

	if (sandbox->state == SANDBOX_RETURNED || sandbox->state == SANDBOX_ERROR) {
		// if (sandbox->remaining_execution == 0) return;

		const uint64_t adjustment = sandbox->remaining_execution;

		new_message.if_case = 11;
		// dbf_try_update_demand(worker_dbf, sandbox->timestamp_of.worker_allocation,
		// sandbox->absolute_deadline-sandbox->timestamp_of.worker_allocation, sandbox->absolute_deadline,
		//                       adjustment, DBF_REDUCE_EXISTING_DEMAND, &new_message);

		if (sandbox->remaining_execution > 0 && !sandbox->exceeded_estimation) {
			// if (!sandbox->exceeded_estimation) {
			dbf_try_update_demand(worker_dbf, sandbox->timestamp_of.request_arrival,
			                      sandbox->module->relative_deadline, sandbox->absolute_deadline,
			                      adjustment, DBF_REDUCE_EXISTING_DEMAND, &new_message);
			// printf("\nWorker %d FINISHED dbf calc for sandbox %lu:\n", worker_thread_idx, sandbox->id);
			// dbf_print(worker_dbf);
		}

		sandbox->remaining_execution -= adjustment;

		new_message.adjustment          = adjustment;
		new_message.dbf_update_mode     = DBF_REDUCE_EXISTING_DEMAND;
		new_message.remaining_execution = sandbox->remaining_execution;

		new_message.if_case = 1;

		if (!ck_ring_enqueue_spsc_message(&cfw->worker_ring, cfw->worker_ring_buffer, &new_message)) {
			panic("Ring The buffer was full and the enqueue operation has failed.!")
		}
		return;
	} else if (sandbox->absolute_deadline <= now) {
		assert(sandbox == current_sandbox_get());
		// if (sandbox->state != SANDBOX_RUNNING_SYS)
		// 	return; /* maybe bring the sandbox to run_sys state if not already? */

		client_socket_send_oneshot(sandbox->client_socket_descriptor, http_header_build(408),
		                           http_header_len(408));
		sandbox->response_code = 4081;
		sandbox_close_http(sandbox);
		current_sandbox_exit();
		return;
	} else if (sandbox->remaining_execution > 0) {
		assert(sandbox == current_sandbox_get());

		const uint64_t adjustment = (sandbox->remaining_execution >= sandbox->last_state_duration)
		                              ? sandbox->last_state_duration
		                              : sandbox->remaining_execution;

		new_message.if_case = 22;
		// dbf_try_update_demand(worker_dbf, sandbox->timestamp_of.worker_allocation,
		// sandbox->absolute_deadline-sandbox->timestamp_of.worker_allocation, sandbox->absolute_deadline,
		//                       adjustment, DBF_REDUCE_EXISTING_DEMAND, &new_message);
		if (!sandbox->exceeded_estimation) {
			dbf_try_update_demand(worker_dbf, sandbox->timestamp_of.request_arrival,
			                      sandbox->module->relative_deadline, sandbox->absolute_deadline,
			                      adjustment, DBF_REDUCE_EXISTING_DEMAND, &new_message);
		}
		sandbox->remaining_execution -= adjustment;

		new_message.adjustment          = adjustment;
		new_message.dbf_update_mode     = DBF_REDUCE_EXISTING_DEMAND;
		new_message.remaining_execution = sandbox->remaining_execution;

		new_message.if_case = 2;
		// printf("Worker %d just executed a little for sandbox %lu!\n\n", worker_thread_idx, sandbox->id);
		if (!ck_ring_enqueue_spsc_message(&cfw->worker_ring, cfw->worker_ring_buffer, &new_message)) {
			panic("Ring The buffer was full and the enqueue operation has failed.!")
		}
	} // else if (sandbox->remaining_execution >= 0) {


	if (sandbox->remaining_execution == 0 && sandbox->state == SANDBOX_INTERRUPTED) {
		/* Going over expected execution!!! */
		assert(sandbox == current_sandbox_get());

		// if (sandbox->has_pending_request_for_extra_demand) {
		// 	// printf("Sandbox #%lu already requested extra demand.\n", sandbox->id);
		// 	// printf("Listener approval did not arrive to worker #%d yet, so shedding sandbox #%lu.\n",
		// 	//        worker_thread_idx, sandbox->id);
		// 	client_socket_send_oneshot(sandbox->client_socket_descriptor, http_header_build(409),
		// 	                           http_header_len(409));
		// 	sandbox->response_code = 4092;
		// 	sandbox_close_http(sandbox);
		// 	current_sandbox_exit();
		// 	return;
		// }

		// uint64_t remaining_worker_supply = dbf_get_available_supply_at(worker_dbf,
		//                                                                sandbox->module->relative_deadline,
		//                                                                sandbox->absolute_deadline,
		//                                                                sandbox->module->reservation_percentile);

		const uint64_t extra_demand = runtime_quantum;

		new_message.if_case = 33;
		// if (remaining_worker_supply > extra_demand) {
		if (dbf_try_update_demand(worker_dbf, now, sandbox->module->relative_deadline,
		                          sandbox->absolute_deadline, extra_demand,
		                          DBF_CHECK_EXISTING_SANDBOX_EXTRA_DEMAND, &new_message)) {
			// new_message.if_case = 33;
			// if (!dbf_try_update_demand(worker_dbf, sandbox->module->relative_deadline,
			//                            sandbox->absolute_deadline, extra_demand,
			//                            DBF_CHECK_EXISTING_SANDBOX_EXTRA_DEMAND, &new_message)) {
			// printf("S_rem_dem=%ld, remaining_worker_supply=%lu, adjust=%ld, ",
			//        sandbox->remaining_execution, remaining_worker_supply, extra_demand);
			// printf("Sandbox state: %u\n", sandbox->state);
			// printf("Sandbox last exec: %lu\n", sandbox->last_state_duration);
			// panic("Worker has supply left, but won't grant extra time to a sandbox!");
			// }
			// printf("Worker %d just granted extra for sandbox %lu!\n", worker_thread_idx, sandbox->id);

			// sandbox->remaining_execution = extra_demand;
			new_message.exceeded_estimation = sandbox->exceeded_estimation = true;
			new_message.adjustment = new_message.remaining_execution = sandbox->remaining_execution =
			  extra_demand;
			new_message.last_extra_demand_timestamp = sandbox->timestamp_of.last_extra_demand_request = now;
			new_message.dbf_update_mode = DBF_CHECK_EXISTING_SANDBOX_EXTRA_DEMAND;

			new_message.prev_rem_exec = 0;
			new_message.last_exec_dur = 0;
			new_message.if_case       = 3;
			// printf("Sending new message to Listener. Its contents: \n\
// 		sandbox->id: %lu \n\
// 		sandbox->pq_idx_in_runqueue: %lu \n",
			// 		       sandbox->id, sandbox->pq_idx_in_runqueue);

			sandbox->has_pending_request_for_extra_demand = true;

			if (!ck_ring_enqueue_spsc_message(&cfw->worker_ring, cfw->worker_ring_buffer, &new_message)) {
				panic("Ring The buffer was full and the enqueue operation has failed.!")
			}

		} else { /* No spare supply is left on the worker, so shed work! (For now drop?!) */
			/* Shed work? Put back to the global queue? */
			assert(sandbox == current_sandbox_get());

			// printf("No supply left in worker #%d. So, shedding sandbox #%lu\n", worker_thread_idx,
			// sandbox->id);
			client_socket_send_oneshot(sandbox->client_socket_descriptor, http_header_build(409),
			                           http_header_len(409));
			sandbox->response_code = 4091;
			sandbox_close_http(sandbox);
			current_sandbox_exit();
			return;
		}
	}
#endif
}
