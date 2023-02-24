#include <threads.h>

#include "current_sandbox.h"
#include "global_request_scheduler.h"
#include "local_runqueue_list.h"
#include "local_runqueue.h"
#include "sandbox_functions.h"
struct FIFO_queue {
	struct ps_list_head local_runqueue_list;
	lock_t lock;
	uint64_t size, capacity;
};

struct FIFO_queue worker_queues_fifo[1024];

bool
local_runqueue_list_is_empty()
{
//	lock_node_t node = {};
//	lock_lock(&worker_queues_fifo[worker_thread_idx].lock, &node);
	bool ret = ps_list_head_empty(&worker_queues_fifo[worker_thread_idx].local_runqueue_list);
//	lock_unlock(&worker_queues_fifo[worker_thread_idx].lock, &node);
	return ret;
}

/* Get the sandbox at the head of the thread local runqueue */
struct sandbox *
local_runqueue_list_get_head()
{	
//	lock_node_t node = {};
//      lock_lock(&worker_queues_fifo[worker_thread_idx].lock, &node);
	struct sandbox *ret = ps_list_head_first_d(&worker_queues_fifo[worker_thread_idx].local_runqueue_list, struct sandbox);
//	lock_unlock(&worker_queues_fifo[worker_thread_idx].lock, &node);
	return ret;
}

/**
 * Removes the sandbox from the thread-local runqueue
 * @param sandbox sandbox
 */
void
local_runqueue_list_remove(struct sandbox *sandbox_to_remove)
{
	worker_queues_fifo[worker_thread_idx].size--;
//	lock_node_t node = {};
//      lock_lock(&worker_queues_fifo[worker_thread_idx].lock, &node);
	ps_list_rem_d(sandbox_to_remove);
//	lock_unlock(&worker_queues_fifo[worker_thread_idx].lock, &node);
}

struct sandbox *
local_runqueue_list_remove_and_return()
{
//	lock_node_t node = {};
//      lock_lock(&worker_queues_fifo[worker_thread_idx].lock, &node);
	struct sandbox *sandbox_to_remove = ps_list_head_first_d(&worker_queues_fifo[worker_thread_idx].local_runqueue_list, struct sandbox);
	ps_list_rem_d(sandbox_to_remove);
//	lock_unlock(&worker_queues_fifo[worker_thread_idx].lock, &node);
	return sandbox_to_remove;
}

/**
 * Append a sandbox to the tail of the runqueue
 * @returns the appended sandbox
 */
void
local_runqueue_list_append(struct sandbox *sandbox_to_append)
{
	assert(sandbox_to_append != NULL);
	assert(ps_list_singleton_d(sandbox_to_append));
//	lock_node_t node = {};
//	lock_lock(&worker_queues_fifo[worker_thread_idx].lock, &node);
	ps_list_head_append_d(&worker_queues_fifo[worker_thread_idx].local_runqueue_list, sandbox_to_append);
//	lock_unlock(&worker_queues_fifo[worker_thread_idx].lock, &node);
}

/* Remove sandbox from head of runqueue and add it to tail */
void
local_runqueue_list_rotate()
{
	/* If runqueue is size one, skip round robin logic since tail equals head */
	if (ps_list_head_one_node(&worker_queues_fifo[worker_thread_idx].local_runqueue_list)) return;
//	lock_node_t node = {};
//      lock_lock(&worker_queues_fifo[worker_thread_idx].lock, &node);
	struct sandbox *sandbox_at_head = local_runqueue_list_remove_and_return();
	assert(sandbox_at_head->state == SANDBOX_INTERRUPTED);
//	local_runqueue_list_append(sandbox_at_head);
//	lock_unlock(&worker_queues_fifo[worker_thread_idx].lock, &node);
}

extern thread_local uint64_t sandbox_added, sandbox_lost;
void
local_runqueue_list_add_index(int index, struct sandbox *sandbox)
{

	if (worker_queues_fifo[index].size > worker_queues_fifo[index].capacity)
		goto free;
	
	ps_list_head_append_d(&worker_queues_fifo[index].local_runqueue_list, sandbox);
	sandbox_added++;
	worker_queues_fifo[index].size++;
	return;
free:
	sandbox->state = SANDBOX_COMPLETE;
	sandbox->http = NULL;
	sandbox_free(sandbox);
	sandbox_lost++;
}


/**
 * Get the next sandbox
 * @return the sandbox to execute or NULL if none are available
 */
struct sandbox *
local_runqueue_list_get_next()
{
	if (local_runqueue_list_is_empty()) return NULL;

	return local_runqueue_list_get_head();
}

void
local_runqueue_list_initialize()
{
	ps_list_head_init(&worker_queues_fifo[worker_thread_idx].local_runqueue_list);
	worker_queues_fifo[worker_thread_idx].size = 0;
	worker_queues_fifo[worker_thread_idx].capacity = 1024;
	/* Register Function Pointers for Abstract Scheduling API */
	lock_init(&worker_queues_fifo[worker_thread_idx].lock);
	struct local_runqueue_config config = { 
						.add_fn_idx = local_runqueue_list_add_index,
						.add_fn      = local_runqueue_list_append,
		                                .is_empty_fn = local_runqueue_list_is_empty,
		                                .delete_fn   = local_runqueue_list_remove,
		                                .get_next_fn = local_runqueue_list_get_next };
	local_runqueue_initialize(&config);
};
