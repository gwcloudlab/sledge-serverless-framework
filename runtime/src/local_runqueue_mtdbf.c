#include <stdint.h>
#include <threads.h>

#include "arch/context.h"
#include "client_socket.h"
#include "current_sandbox.h"
#include "debuglog.h"
#include "global_request_scheduler.h"
#include "local_runqueue.h"
#include "local_runqueue_mtdbf.h"
#include "panic.h"
#include "priority_queue.h"
#include "sandbox_functions.h"
#include "runtime.h"
#include "dbf.h"

thread_local static struct priority_queue *local_runqueue_mtdbf;

// thread_local static int max_local_runqueue_len = 0; //////////

/**
 * Checks if the run queue is empty
 * @returns true if empty. false otherwise
 */
bool
local_runqueue_mtdbf_is_empty()
{
	return priority_queue_length_nolock(local_runqueue_mtdbf) == 0;
}

/**
 * Adds a sandbox to the run queue
 * @param sandbox
 * @returns pointer to sandbox added
 */
void
local_runqueue_mtdbf_add(struct sandbox *sandbox)
{
	assert(sandbox != NULL);

	/* Add the sandbox to the per-worker-module (pwm) queue */
	int rc = priority_queue_enqueue_nolock(local_runqueue_mtdbf, sandbox);
	if (unlikely(rc == -ENOSPC)) {
		struct priority_queue *temp = priority_queue_grow_nolock(local_runqueue_mtdbf);
		if (unlikely(temp == NULL)) panic("Failed to grow local runqueue\n");
		local_runqueue_mtdbf = temp;
		rc                   = priority_queue_enqueue_nolock(local_runqueue_mtdbf, sandbox);
		if (unlikely(rc == -ENOSPC)) panic("Thread Runqueue is full!\n");
	}

	sandbox->timestamp_of.worker_allocation = __getcycles();
	sandbox->owned_worker_idx               = worker_thread_idx;

	// if(priority_queue_length_nolock(local_runqueue_mtdbf) > max_local_runqueue_len) {
	// 	max_local_runqueue_len = priority_queue_length_nolock(local_runqueue_mtdbf);
	// 	debuglog("Local MAX Queue Length: %u", max_local_runqueue_len);
	// }
}

/**
 * Deletes a sandbox from the runqueue
 * @param sandbox to delete
 */
static void
local_runqueue_mtdbf_delete(struct sandbox *sandbox)
{
	assert(sandbox != NULL);

	priority_queue_delete_by_idx_nolock(local_runqueue_mtdbf, sandbox, sandbox->pq_idx_in_runqueue);
	// sandbox->owned_worker_idx               = -2;
}

/**
 * This function determines the next sandbox to run.
 * This is the head of the runqueue
 *
 * Execute the sandbox at the head of the thread local runqueue
 * @return the sandbox to execute or NULL if none are available
 */
struct sandbox *
local_runqueue_mtdbf_get_next()
{
	/* Get the deadline of the sandbox at the head of the local request queue */
	struct sandbox *next = NULL;
	int             rc   = priority_queue_top_nolock(local_runqueue_mtdbf, (void **)&next);

	if (rc == -ENOENT) return NULL;

	return next;
}

/**
 * Registers the PS variant with the polymorphic interface
 */
void
local_runqueue_mtdbf_initialize()
{
	/* Initialize local state */
	local_runqueue_mtdbf = priority_queue_initialize(4096, false, ////// TODO fix size
	                                                 sandbox_get_priority, local_runqueue_update_highest_priority, sandbox_update_pq_idx_in_runqueue);

	// LocalQueues[worker_thread_idx] = local_runqueue_mtdbf;

	/* Register Function Pointers for Abstract Scheduling API */
	struct local_runqueue_config config = { .add_fn      = local_runqueue_mtdbf_add,
		                                .is_empty_fn = local_runqueue_mtdbf_is_empty,
		                                .delete_fn   = local_runqueue_mtdbf_delete,
		                                .get_next_fn = local_runqueue_mtdbf_get_next };

	local_runqueue_initialize(&config);
}


size_t
queue_length()
{
	return priority_queue_length_nolock(local_runqueue_mtdbf);
}
