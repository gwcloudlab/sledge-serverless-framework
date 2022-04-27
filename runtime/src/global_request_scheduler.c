#include <stdnoreturn.h>

#include "global_request_scheduler.h"
#include "panic.h"

/* Default uninitialized implementations of the polymorphic interface */
noreturn static struct sandbox *
uninitialized_add(struct sandbox *arg)
{
	panic("Global Request Scheduler Add was called before initialization\n");
}

noreturn static int
uninitialized_remove(struct sandbox **arg)
{
	panic("Global Request Scheduler Remove was called before initialization\n");
}

noreturn static uint64_t
uninitialized_peek()
{
	panic("Global Request Scheduler Peek was called before initialization\n");
}


/* The global of our polymorphic interface */
static struct global_request_scheduler_config global_request_scheduler = { .add_fn    = uninitialized_add,
	                                                                   .remove_fn = uninitialized_remove,
	                                                                   .peek_fn   = uninitialized_peek };

/**
 * Initializes the polymorphic interface with a concrete implementation
 * @param config
 */
void
global_request_scheduler_initialize(struct global_request_scheduler_config *config)
{
	assert(config != NULL);
	memcpy(&global_request_scheduler, config, sizeof(struct global_request_scheduler_config));
}


/**
 * Adds a sandbox to the request scheduler
 * @param sandbox
 * @returns pointer to sandbox if added. NULL otherwise
 */
struct sandbox *
global_request_scheduler_add(struct sandbox *sandbox)
{
	assert(sandbox != NULL);
	return global_request_scheduler.add_fn(sandbox);
}

/**
 * Removes a sandbox according to the scheduling policy of the variant
 * @param removed_sandbox where to write the adddress of the removed sandbox
 * @returns 0 if successfully returned a sandbox, -ENOENT if empty, -EAGAIN if atomic operation unsuccessful
 */
int
global_request_scheduler_remove(struct sandbox **removed_sandbox)
{
	assert(removed_sandbox != NULL);
	return global_request_scheduler.remove_fn(removed_sandbox);
}

/**
 * Removes a sandbox according to the scheduling policy of the variant
 * @param removed_sandbox where to write the adddress of the removed sandbox
 * @param target_deadline the deadline that must be validated before dequeuing
 * @returns 0 if successfully returned a sandbox, -ENOENT if empty or if no element meets target_deadline,
 * -EAGAIN if atomic operation unsuccessful
 */
int
global_request_scheduler_remove_if_earlier(struct sandbox **removed_sandbox, uint64_t target_deadline)
{
	assert(removed_sandbox != NULL);
	return global_request_scheduler.remove_if_earlier_fn(removed_sandbox, target_deadline);
}

/**
 * Removes a sandbox request according to the scheduling policy of the variant
 * @param removed_sandbox where to write the adddress of the removed sandbox
 * @param target_deadline the deadline that must be validated before dequeuing
 * @param mt_class the multi-tenancy class of the global request to compare the target deadline against
 * @returns 0 if successfully returned a sandbox request, -ENOENT if empty or if no element meets target_deadline,
 * -EAGAIN if atomic operation unsuccessful
 */
int
global_request_scheduler_remove_with_mt_class(struct sandbox **removed_sandbox, uint64_t target_deadline,
                                              enum MULTI_TENANCY_CLASS mt_class)
{
	assert(removed_sandbox != NULL);
	return global_request_scheduler.remove_with_mt_class_fn(removed_sandbox, target_deadline, mt_class);
}

/**
 * Peeks at the priority of the highest priority sandbox
 * @returns highest priority
 */
uint64_t
global_request_scheduler_peek()
{
	return global_request_scheduler.peek_fn();
}
