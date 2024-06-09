#include <stdint.h>
#include <threads.h>

#include "software_interrupt.h"
#include "arch/context.h"
#include "current_sandbox.h"
#include "debuglog.h"
#include "global_request_scheduler.h"
#include "local_runqueue.h"
#include "local_runqueue_binary_tree.h"
#include "panic.h"
#include "binary_search_tree.h"
#include "sandbox_functions.h"
#include "runtime.h"
#include "memlogging.h"

extern thread_local uint8_t dispatcher_thread_idx;
extern _Atomic uint32_t local_queue_length[1024];
extern uint32_t max_local_queue_length[1024];
extern uint32_t max_local_queue_height[1024];
extern bool runtime_exponential_service_time_simulation_enabled;
extern thread_local int global_worker_thread_idx;
extern struct sandbox* current_sandboxes[1024];
extern struct binary_tree *worker_binary_trees[1024];
thread_local static struct binary_tree *local_runqueue_binary_tree = NULL;

/**
 * Checks if the run queue is empty
 * @returns true if empty. false otherwise
 */
bool
local_runqueue_binary_tree_is_empty()
{
	assert(local_runqueue_binary_tree != NULL);

	return is_empty(local_runqueue_binary_tree);
}

/**
 * Checks if the run queue is empty
 * @returns true if empty. false otherwise
 */
bool
local_runqueue_binary_tree_is_empty_index(int index)
{
	struct binary_tree *binary_tree = worker_binary_trees[index];

        assert(binary_tree != NULL);

        return is_empty(binary_tree);
}

/**
 * Adds a sandbox to the run queue
 * @param sandbox
 * @returns pointer to sandbox added
 */
void
local_runqueue_binary_tree_add(struct sandbox *sandbox)
{
	assert(sandbox != NULL);

	lock_node_t node_lock = {};
    	lock_lock(&local_runqueue_binary_tree->lock, &node_lock);
	local_runqueue_binary_tree->root = insert(local_runqueue_binary_tree, local_runqueue_binary_tree->root, sandbox, global_worker_thread_idx);
	lock_unlock(&local_runqueue_binary_tree->lock, &node_lock);
}

void
local_runqueue_binary_tree_add_index(int index, struct sandbox *sandbox)
{
	assert(sandbox != NULL);

	struct binary_tree *binary_tree = worker_binary_trees[index];
	lock_node_t node_lock = {};
	lock_lock(&binary_tree->lock, &node_lock);
	binary_tree->root = insert(binary_tree, binary_tree->root, sandbox, index);
	lock_unlock(&binary_tree->lock, &node_lock);
       
        /*int height = findHeight(binary_tree->root); 
        if ( height > max_local_queue_height[index]) {
            max_local_queue_height[index] = height;
        }*/

	atomic_fetch_add(&local_queue_length[index], 1);
	if (local_queue_length[index] > max_local_queue_length[index]) {
		max_local_queue_length[index] = local_queue_length[index];
		/*mem_log("listener %d 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u 8:%u 9:%u\n", dispatcher_thread_idx, max_local_queue_length[0],
			max_local_queue_length[1],max_local_queue_length[2],
			max_local_queue_length[3],max_local_queue_length[4], max_local_queue_length[5], max_local_queue_length[6],
			max_local_queue_length[7], max_local_queue_length[8]);
		*/
	}

	/* Set estimated exeuction time for the sandbox */
	if (runtime_exponential_service_time_simulation_enabled == false) {
        	uint32_t uid = sandbox->route->admissions_info.uid;
        	uint64_t estimated_execute_cost = perf_window_get_percentile(&worker_perf_windows[index][uid],
                                                                     sandbox->route->admissions_info.percentile,
                                                                     sandbox->route->admissions_info.control_index);
        	/* Use expected execution time in the configuration file as the esitmated execution time 
           	   if estimated_execute_cost is 0 
         	*/
        	if (estimated_execute_cost == 0) {
            		estimated_execute_cost = sandbox->route->expected_execution_cycle;
		} 
        	sandbox->estimated_cost = estimated_execute_cost;
		sandbox->relative_deadline = sandbox->route->relative_deadline;
		sandbox->max_running_cycles = sandbox->relative_deadline << RUNTIME_MAX_RUNNING_TIME_COEFFICIENT;
	}
	/* Record TS and calcuate RS. SRSF algo:
           1. When reqeust arrives to the queue, record TS and calcuate RS. RS = deadline - execution time
           2. When request starts running, update RS
           3. When request stops, update TS
           4. When request resumes, update RS 
        */
	sandbox->srsf_stop_running_ts = __getcycles();
	sandbox->srsf_remaining_slack = sandbox->relative_deadline - sandbox->estimated_cost;
	worker_queuing_cost_increment(index, sandbox->estimated_cost);
}

/**
 * Deletes a sandbox from the runqueue
 * @param sandbox to delete
 */
static void
local_runqueue_binary_tree_delete(struct sandbox *sandbox)
{
	assert(sandbox != NULL);

	lock_node_t node_lock = {};
	lock_lock(&local_runqueue_binary_tree->lock, &node_lock);
	bool deleted = false;
	local_runqueue_binary_tree->root = delete_i(local_runqueue_binary_tree, local_runqueue_binary_tree->root, sandbox, &deleted, global_worker_thread_idx);
	lock_unlock(&local_runqueue_binary_tree->lock, &node_lock);
	if (deleted == false) { 
		panic("Tried to delete sandbox %lu state %d from runqueue %p, but was not present\n", 
		       sandbox->id, sandbox->state, local_runqueue_binary_tree);
	}

	atomic_fetch_sub(&local_queue_length[global_worker_thread_idx], 1);
	worker_queuing_cost_decrement(global_worker_thread_idx, sandbox->estimated_cost);	
}

/**
 * This function determines the next sandbox to run.
 * This is the head of the local runqueue
 *
 * Execute the sandbox at the head of the thread local runqueue
 * @return the sandbox to execute or NULL if none are available
 */
struct sandbox *
local_runqueue_binary_tree_get_next()
{
	/* Get the minimum deadline of the sandbox of the local request queue */
	struct TreeNode *node = findMin(local_runqueue_binary_tree, local_runqueue_binary_tree->root);
	if (node != NULL) {
		return node->data;
	} else {
		return NULL;
	}
}

/**
 * Try but not real add a item to the local runqueue.
 * @param index The worker thread id
 * @param sandbox Try to add 
 * @returns The waiting serving time in cycles for this sandbox if adding it to the queue
 */
uint64_t 
local_runqueue_binary_tree_try_add_index(int index, struct sandbox *sandbox, bool *need_interrupt)
{
	struct binary_tree *binary_tree = worker_binary_trees[index];
	assert(binary_tree != NULL);
	*need_interrupt = false;

	if (is_empty(binary_tree)) {
		/* The worker is idle */
		return 0;
	} else if (current_sandboxes[index] != NULL &&
		   current_sandboxes[index]->srsf_remaining_slack > 0 && 
		   sandbox_is_preemptable(current_sandboxes[index]) == true && 
		   sandbox_get_priority(sandbox) < sandbox_get_priority(current_sandboxes[index])) {
		/* The new one has a higher priority than the current one, need to interrupt the current one */
		*need_interrupt = true;
		return 0;
	} else {
		/* Current sandbox cannot be interrupted because its priority is higher or its RS is 0, just find
                   a right location to add the new sandbox to the tree 
		*/
		uint64_t waiting_serving_time = 0;
		lock_node_t node_lock = {};
    		lock_lock(&binary_tree->lock, &node_lock);
		waiting_serving_time = findMaxValueLessThan(binary_tree, binary_tree->root, sandbox, index);
		lock_unlock(&binary_tree->lock, &node_lock);
                //printf("worker %d waiting time is %lu\n", index, waiting_serving_time);
		return waiting_serving_time; 
	}

}

int local_runqueue_binary_tree_get_height() {
	assert (local_runqueue_binary_tree != NULL);
	return findHeight(local_runqueue_binary_tree->root); 
}

int local_runqueue_binary_tree_get_length() {
	assert (local_runqueue_binary_tree != NULL);
	return getNonDeletedNodeCount(local_runqueue_binary_tree); 
}

int local_runqueue_binary_tree_get_length_index(int index) {
	struct binary_tree *binary_tree = worker_binary_trees[index];
        assert(binary_tree != NULL);

        return getNonDeletedNodeCount(binary_tree);
}

void local_runqueue_print_in_order(int index) {
	struct binary_tree *binary_tree = worker_binary_trees[index];
	assert(binary_tree != NULL);
	print_tree_in_order(binary_tree);
}

/**
 * Registers the PS variant with the polymorphic interface
 */
void
local_runqueue_binary_tree_initialize()
{
	/* Initialize local state */
	local_runqueue_binary_tree = init_binary_tree(true, sandbox_get_priority, sandbox_get_execution_cost, global_worker_thread_idx, 4096);

	worker_binary_trees[global_worker_thread_idx] = local_runqueue_binary_tree;
	/* Register Function Pointers for Abstract Scheduling API */
	struct local_runqueue_config config = { .add_fn         = local_runqueue_binary_tree_add,
						.add_fn_idx     = local_runqueue_binary_tree_add_index,
						.try_add_fn_idx = local_runqueue_binary_tree_try_add_index,
		                                .is_empty_fn    = local_runqueue_binary_tree_is_empty,
						.is_empty_fn_idx = local_runqueue_binary_tree_is_empty_index,
		                                .delete_fn      = local_runqueue_binary_tree_delete,
		                                .get_next_fn    = local_runqueue_binary_tree_get_next,
					        .get_height_fn  = local_runqueue_binary_tree_get_height,
						.get_length_fn  = local_runqueue_binary_tree_get_length,
						.get_length_fn_idx  = local_runqueue_binary_tree_get_length_index,
						.print_in_order_fn_idx = local_runqueue_print_in_order
					      };

	local_runqueue_initialize(&config);
}
