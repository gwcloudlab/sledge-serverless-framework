#include <assert.h>
#include <errno.h>

#include "global_request_scheduler.h"
#include "listener_thread.h"
#include "panic.h"
#include "priority_queue.h"
#include "runtime.h"
#include "dbf.h"
#include "module_database.h"

static struct priority_queue     *global_request_scheduler_mtdbf;
extern thread_local struct sandbox_metadata *sandbox_meta;

lock_t global_lock;
// int           max_global_runqueue_len = 0; //////////

/**
 * @brief Check if any of the requests in the global queue has already missed its deadline
 *  and kill such if so.
 * @return number of dead requests that got cleared from the global queue
 */
static uint16_t
global_request_scheduler_mtdbf_clear_dead_requests_nolock()
{
	// if (unlikely(!listener_thread_is_running())) panic("%s is only callable by the listener thread\n", __func__);
	const uint64_t now        = __getcycles();
	uint16_t       count_dead = 0;

	while (global_request_scheduler_peek() < now) {
		struct sandbox *sandbox_to_remove = NULL;
		priority_queue_dequeue_nolock(global_request_scheduler_mtdbf, (void **)&sandbox_to_remove);
		assert(sandbox_to_remove);

		assert(sandbox_to_remove->remaining_execution >= 0);

		if (sandbox_to_remove->state == SANDBOX_INITIALIZED) {
			struct module *module = sandbox_to_remove->module;
			priority_queue_delete_by_idx_nolock(module->global_sandboxes, sandbox_to_remove,
			                                    sandbox_to_remove->pq_idx_in_module_queue);

			struct message temp_message = { .module        = module,
				                        .if_case       = 77,
				                        .reserv        = module->reservation_percentile,
				                        .prev_rem_exec = sandbox_to_remove->remaining_execution,
				                        .state         = sandbox_to_remove->state };

			dbf_try_update_demand(module->module_dbf, sandbox_to_remove->timestamp_of.request_arrival,
			                      module->relative_deadline, sandbox_to_remove->absolute_deadline,
			                      sandbox_to_remove->remaining_execution, DBF_REDUCE_EXISTING_DEMAND,
			                      &temp_message);

			temp_message.if_case = 88;
			dbf_try_update_demand(global_dbf, sandbox_to_remove->timestamp_of.request_arrival,
			                      module->relative_deadline, sandbox_to_remove->absolute_deadline,
			                      sandbox_to_remove->remaining_execution, DBF_REDUCE_EXISTING_DEMAND,
			                      &temp_message);
		}


		client_socket_send_oneshot(sandbox_to_remove->client_socket_descriptor, http_header_build(408),
		                           http_header_len(408));
		client_socket_close(sandbox_to_remove->client_socket_descriptor, &sandbox_to_remove->client_address);
		sandbox_to_remove->timestamp_of.worker_allocation = now;
		sandbox_to_remove->response_code                  = 4080;

		sandbox_set_as_error(sandbox_to_remove, sandbox_to_remove->state);
		sandbox_free(sandbox_to_remove);

		count_dead++;
	}

	return count_dead;
}

/**
 * Pushes a sandbox request to the global runqueue
 * @param sandbox
 * @returns pointer to request if added. NULL otherwise
 */
static struct sandbox *
global_request_scheduler_mtdbf_add(struct sandbox *sandbox)
{
	assert(sandbox);
	assert(global_request_scheduler_mtdbf);
	// if (unlikely(!listener_thread_is_running())) panic("%s is only callable by the listener thread\n", __func__);

	LOCK_LOCK(&global_lock);

	if (listener_thread_is_running()) {
		assert(sandbox->state == SANDBOX_INITIALIZED);
		uint16_t count_dead = global_request_scheduler_mtdbf_clear_dead_requests_nolock();
		// if(count_dead > 0) {
		// 	printf ("Listener just cleared %u dead requests\n", count_dead);
		// }
	}


	int rc = priority_queue_enqueue_nolock(global_request_scheduler_mtdbf, sandbox);
	if (rc != 0) {
		sandbox = NULL;
	} else if (sandbox->state == SANDBOX_INITIALIZED) {
		assert(listener_thread_is_running());
		rc = priority_queue_enqueue_nolock(sandbox->module->global_sandboxes, sandbox);
		assert(rc == 0);
	}

	// if(priority_queue_length_nolock(global_request_scheduler_mtdbf) > max_global_runqueue_len) {
	// 	max_global_runqueue_len = priority_queue_length_nolock(global_request_scheduler_mtdbf);
	// 	debuglog("Global MAX Queue Length: %u", max_global_runqueue_len);
	// }
	sandbox->owned_worker_idx = -1;

	LOCK_UNLOCK(&global_lock);

	return sandbox;
}

/**
 * @param pointer to the pointer that we want to set to the address of the removed sandbox request
 * @returns 0 if successful, -ENOENT if empty
 */
int
global_request_scheduler_mtdbf_remove(struct sandbox **removed_sandbox)
{
	/* This function won't be used with the MTDS scheduler. Keeping merely for the polymorhism. */
	return -1;
}

/**
 * @param removed_sandbox pointer to set to removed sandbox request
 * @param target_deadline the deadline that the request must be earlier than to dequeue
 * @returns 0 if successful, -ENOENT if empty or if request isn't earlier than target_deadline
 */
int
global_request_scheduler_mtdbf_remove_if_earlier(struct sandbox **removed_sandbox, uint64_t target_deadline)
{
	int rc;

	if (sandbox_meta->module) {
		sandbox_meta         = malloc(sizeof(struct sandbox_metadata));
		sandbox_meta->module = NULL;
	}

	LOCK_LOCK(&global_lock);

	/* Avoid unnessary locks when the target_deadline is tighter than the head of the Global runqueue */
	uint64_t global_deadline = global_request_scheduler_peek();

	if (global_deadline >= target_deadline) goto err_enoent;

	/* Spot the sandbox to remove */
	rc = priority_queue_top_nolock(global_request_scheduler_mtdbf, (void **)removed_sandbox);
	assert(*removed_sandbox);
	assert((*removed_sandbox)->absolute_deadline == global_deadline);

	struct module *module       = (*removed_sandbox)->module;
	struct message temp_message = { .module        = module,
		                        .if_case       = 17,
		                        .reserv        = module->reservation_percentile,
		                        .prev_rem_exec = (*removed_sandbox)->remaining_execution,
		                        .state         = (*removed_sandbox)->state };
	// uint64_t now = __getcycles();

	dbf_update_mode_t mode      = DBF_CHECK_AND_ADD_DEMAND;
	uint64_t          adjusment = (*removed_sandbox)->remaining_execution;

	if ((*removed_sandbox)->state != SANDBOX_INITIALIZED) {
		assert(adjusment == 0);
		mode      = DBF_CHECK_EXISTING_SANDBOX_EXTRA_DEMAND;
		adjusment = runtime_quantum;
	}

	if (!dbf_try_update_demand(worker_dbf, (*removed_sandbox)->timestamp_of.request_arrival,
	                           module->relative_deadline, global_deadline, adjusment, mode, &temp_message)) {
		assert(worker_dbf->worker_idx == worker_thread_idx);
		// fprintf(stderr, "Worker %i DENIED a job with demand %ld!\n", worker_thread_idx,
		// (*removed_sandbox)->remaining_execution); fprintf(stderr, "Global DL=%lu, Local DL=%lu!\n",
		// global_deadline, target_deadline); dbf_print(worker_dbf); exit(-1);
		goto err_enoent;
	}
	// (*removed_sandbox)->timestamp_of.worker_allocation = now; // rmeove the same op from scheduler validate and
	// set_as_runable printf("Worker %i accpted a sandbox #%lu!\n", worker_thread_idx, (*removed_sandbox)->id);
	// dbf_print(worker_dbf);

	struct sandbox *removed_sandbox_temp = NULL;
	rc = priority_queue_dequeue_nolock(global_request_scheduler_mtdbf, (void **)&removed_sandbox_temp);
	assert(rc == 0);
	assert(*removed_sandbox == removed_sandbox_temp);
	(*removed_sandbox)->owned_worker_idx = -2;

	if (removed_sandbox_temp->state == SANDBOX_INITIALIZED) {
		priority_queue_delete_by_idx_nolock(module->global_sandboxes, removed_sandbox_temp,
		                                    removed_sandbox_temp->pq_idx_in_module_queue);

		// struct sandbox_metadata *sandbox_meta = malloc(sizeof(struct sandbox_metadata));

		/* TODO: sandbox_meta_init */
		sandbox_meta->sandbox_shadow       = removed_sandbox_temp;
		sandbox_meta->module               = module;
		sandbox_meta->id                   = removed_sandbox_temp->id;
		sandbox_meta->arrival_timestamp    = removed_sandbox_temp->timestamp_of.request_arrival;
		sandbox_meta->absolute_deadline    = removed_sandbox_temp->absolute_deadline;
		sandbox_meta->remaining_execution  = removed_sandbox_temp->remaining_execution;
		sandbox_meta->exceeded_estimation  = removed_sandbox_temp->exceeded_estimation;
		sandbox_meta->owned_worker_idx     = worker_thread_idx;
		sandbox_meta->terminated           = false;
		removed_sandbox_temp->sandbox_meta = sandbox_meta;
		rc = priority_queue_enqueue_nolock(removed_sandbox_temp->module->local_sandbox_metas, sandbox_meta);
		assert(rc == 0);
	}

	// if(priority_queue_length_nolock(global_request_scheduler_mtdbf) == 0) {
	// 	printf("\n\n WORKER %d EMPTY ", worker_thread_idx);
	// 	dbf_print(worker_dbf);

	// 	printf("\n\n GLOBAL EMPTY ");
	// 	dbf_print(global_dbf);

	// 	printf("MODULE %s - EMPTY ", module->name);
	// 	dbf_print(module->module_dbf);
	// }

done:
	LOCK_UNLOCK(&global_lock);
	return rc;
err_enoent:
	rc = -ENOENT;
	goto done;
}

/**
 * @param removed_sandbox pointer to set to removed sandbox request
 * @param target_deadline the deadline that the request must be earlier than to dequeue
 * @param mt_class the multi-tenancy class of the global request to compare the target deadline against
 * @returns 0 if successful, -ENOENT if empty or if request isn't earlier than target_deadline
 */
int
global_request_scheduler_mtdbf_remove_with_mt_class(struct sandbox **removed_sandbox, uint64_t target_deadline,
                                                    enum MULTI_TENANCY_CLASS target_mt_class)
{
	/* This function won't be used with the MTDBF scheduler. Keeping merely for the polymorhism. */
	return -1;
}

/**
 * Peek at the priority of the highest priority task without having to take the lock
 * Because this is a min-heap PQ, the highest priority is the lowest 64-bit integer
 * This is used to store an absolute deadline
 * @returns value of highest priority value in queue or ULONG_MAX if empty
 */
static uint64_t
global_request_scheduler_mtdbf_peek(void)
{
	return priority_queue_peek(global_request_scheduler_mtdbf);
}


/**
 * Initializes the variant and registers against the polymorphic interface
 */
void
global_request_scheduler_mtdbf_initialize()
{
	global_request_scheduler_mtdbf = priority_queue_initialize(4096, false, sandbox_get_priority, global_request_scheduler_update_highest_priority,
	                                                           sandbox_update_pq_idx_in_runqueue);

	LOCK_INIT(&global_lock);

	struct global_request_scheduler_config config = {
		.add_fn                  = global_request_scheduler_mtdbf_add,
		.remove_fn               = global_request_scheduler_mtdbf_remove,
		.remove_if_earlier_fn    = global_request_scheduler_mtdbf_remove_if_earlier,
		.remove_with_mt_class_fn = global_request_scheduler_mtdbf_remove_with_mt_class,
		.peek_fn                 = global_request_scheduler_mtdbf_peek
	};

	global_request_scheduler_initialize(&config);
}

void
global_request_scheduler_mtdbf_free()
{
	priority_queue_free(global_request_scheduler_mtdbf);
}

/**
 * @brief Shed work from a tenant that has gone over its allowed supply the most
 *
 * @param module_to_punish pointer to the module of whose sandbox will be removed from the global queue
 * @returns 0 if successfully shed work, -ENOENT otherwise (which means all this tenant's work is already in the local
 * queues)
 */
int
global_request_scheduler_mtdbf_shed_work(struct module *bad_module_to_punish, uint64_t request_arrival_timestamp,
                                         uint64_t absolute_deadline)
{
	LOCK_LOCK(&global_lock);

	struct module *module_to_punish    = NULL;
	uint64_t       max_demand_overgone = 0;
	int            rc                  = -1;

	for (size_t i = 0; i < module_database_count; i++) {
		struct module *module = module_database[i];
		assert(module);

		uint64_t demand_overgone = dbf_get_demand_overgone_its_supply_at(module->module_dbf,
		                                                                 request_arrival_timestamp,
		                                                                 absolute_deadline);
		if (demand_overgone > max_demand_overgone) {
			max_demand_overgone = demand_overgone;
			module_to_punish    = module;
		}
	}

	// if (!module_to_punish) {
	// 	for (size_t i = 0; i < module_database_count; i++) {
	// 		struct module *module = module_database[i];
	// 		assert(module);
	// 		uint64_t demand_overgone = dbf_get_demand_overgone_its_supply_at(module->module_dbf,
	// 		                                                                 request_arrival_timestamp,
	// 		                                                                 absolute_deadline);
	// 		printf("rp=%u, demand_overgone=%lu, max_demand_overgone=%lu\n",
	// 		       module->reservation_percentile, demand_overgone, max_demand_overgone);
	// 		if (demand_overgone > max_demand_overgone) {
	// 			max_demand_overgone = demand_overgone;
	// 			module_to_punish    = module;
	// 		}
	// 	}
	// 	assert(1);
	// }
	if (!module_to_punish) goto done;
	// assert(module_to_punish);

	// if (module_to_punish->reservation_percentile != 0) {
	// 	printf("rp=%u, max_demand_overgone=%lu\n", module_to_punish->reservation_percentile,
	// 	       max_demand_overgone);
	// }
	// assert(module_to_punish->reservation_percentile == 0); ///// temp
	// printf("Module to punish is: %s\n", module_to_punish->name);


	struct sandbox *sandbox_to_remove = NULL;

	rc = priority_queue_dequeue_nolock(module_to_punish->global_sandboxes, (void **)&sandbox_to_remove);
	if (rc != 0) {
		struct sandbox_metadata *sandbox_meta = NULL;
		// printf("PEEK = %lu\n", priority_queue_peek(module_to_punish->local_sandbox_metas));
		rc = priority_queue_dequeue_nolock(module_to_punish->local_sandbox_metas, (void **)&sandbox_meta);
		if (rc != 0) {
			// printf("Sizes: global_queue=%d,  module_glbl=%d, module_local=%d\n",
			//    priority_queue_length_nolock(global_request_scheduler_mtdbf),
			//    priority_queue_length_nolock(module_to_punish->global_sandboxes),
			//    priority_queue_length_nolock(module_to_punish->local_sandbox_metas));
			goto done;
		}
		assert(sandbox_meta);

		// if (sandbox_meta->terminated) {
		// 	printf ("Sandbox already terminated!\n");
		// 	// rc = -1;
		// 	goto done;
		// }
		// assert(rc == 0);
		assert(sandbox_meta->terminated == false);
		sandbox_meta->terminated = true;

		// if (!sandbox_refs[sandbox_meta->id%RUNTIME_MAX_ALIVE_SANDBOXES]) {
		// 	printf ("Sandbox does not exist anymore!\n");
		// 	goto done;
		// }

		if (sandbox_meta->remaining_execution < runtime_quantum/2) {
			printf("Not worth killing this sandbox!\n");
			// rc = -1;
			goto done;
		}

		assert(comm_to_workers);
		struct comm_with_worker *ctw = &comm_to_workers[sandbox_meta->owned_worker_idx];
		assert(ctw);
		assert(ctw->worker_idx == sandbox_meta->owned_worker_idx);
		assert(ck_ring_size(&ctw->worker_ring) < LISTENER_THREAD_RING_SIZE);

		struct message new_message                = { 0 };
		new_message.sandbox                       = sandbox_meta->sandbox_shadow;
		new_message.sandbox_id                    = sandbox_meta->id;
		// new_message.extra_demand_request_approved = false;

		new_message.if_case = 555;
		dbf_try_update_demand(module_to_punish->module_dbf,
		                      sandbox_meta->absolute_deadline - module_to_punish->relative_deadline,
		                      module_to_punish->relative_deadline, sandbox_meta->absolute_deadline,
		                      sandbox_meta->remaining_execution, DBF_REDUCE_EXISTING_DEMAND, &new_message);

		new_message.if_case = 556;
		dbf_try_update_demand(global_dbf, sandbox_meta->absolute_deadline - module_to_punish->relative_deadline,
		                      module_to_punish->relative_deadline, sandbox_meta->absolute_deadline,
		                      sandbox_meta->remaining_execution, DBF_REDUCE_EXISTING_DEMAND, &new_message);

		if (!ck_ring_enqueue_spsc_message(&ctw->worker_ring, ctw->worker_ring_buffer, &new_message)) {
			panic("Ring The buffer was full and the enqueue "
			      "operation has failed.!")
		}

		pthread_kill(runtime_worker_threads[sandbox_meta->owned_worker_idx], SIGALRM);

		goto done;
	} else {
		priority_queue_delete_by_idx_nolock(global_request_scheduler_mtdbf, sandbox_to_remove,
		                                    sandbox_to_remove->pq_idx_in_runqueue);
	}
	assert(sandbox_to_remove);

	struct message temp_message = { .module        = module_to_punish,
		                        .if_case       = 99,
		                        .reserv        = module_to_punish->reservation_percentile,
		                        .prev_rem_exec = sandbox_to_remove->remaining_execution,
		                        .state         = sandbox_to_remove->state };

	assert(sandbox_to_remove->state == SANDBOX_INITIALIZED);
	assert(sandbox_to_remove->remaining_execution > 0);

	dbf_try_update_demand(module_to_punish->module_dbf, sandbox_to_remove->timestamp_of.request_arrival,
	                      module_to_punish->relative_deadline, sandbox_to_remove->absolute_deadline,
	                      sandbox_to_remove->remaining_execution, DBF_REDUCE_EXISTING_DEMAND, &temp_message);

	dbf_try_update_demand(global_dbf, sandbox_to_remove->timestamp_of.request_arrival,
	                      module_to_punish->relative_deadline, sandbox_to_remove->absolute_deadline,
	                      sandbox_to_remove->remaining_execution, DBF_REDUCE_EXISTING_DEMAND, &temp_message);


	client_socket_send_oneshot(sandbox_to_remove->client_socket_descriptor, http_header_build(409),
	                           http_header_len(409));
	client_socket_close(sandbox_to_remove->client_socket_descriptor, &sandbox_to_remove->client_address);
	sandbox_to_remove->timestamp_of.worker_allocation = __getcycles();
	sandbox_to_remove->response_code                  = 4090;
	// generic_thread_dump_lock_overhead(); // TODO Sean, this necessary here?
	sandbox_set_as_error(sandbox_to_remove, sandbox_to_remove->state);
	sandbox_free(sandbox_to_remove);

done:
	LOCK_UNLOCK(&global_lock);
	return rc;
}
