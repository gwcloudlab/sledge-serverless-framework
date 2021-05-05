#include "current_sandbox.h"

/* current sandbox that is active.. */
static __thread struct sandbox *worker_thread_current_sandbox = NULL;

__thread struct sandbox_context_cache local_sandbox_context_cache = {
	.linear_memory_start   = NULL,
	.linear_memory_size    = 0,
	.module_indirect_table = NULL,
};

/**
 * Getter for the current sandbox executing on this thread
 * @returns the current sandbox executing on this thread
 */
struct sandbox *
current_sandbox_get(void)
{
	return worker_thread_current_sandbox;
}

/**
 * Setter for the current sandbox executing on this thread
 * @param sandbox the sandbox we are setting this thread to run
 */
void
current_sandbox_set(struct sandbox *sandbox)
{
	/* Unpack hierarchy to avoid pointer chasing */
	if (sandbox == NULL) {
		local_sandbox_context_cache = (struct sandbox_context_cache){
			.linear_memory_start   = NULL,
			.linear_memory_size    = 0,
			.module_indirect_table = NULL,
		};
		worker_thread_current_sandbox = NULL;
	} else {
		local_sandbox_context_cache = (struct sandbox_context_cache){
			.linear_memory_start   = sandbox->linear_memory_start,
			.linear_memory_size    = sandbox->linear_memory_size,
			.module_indirect_table = sandbox->module->indirect_table,
		};
		worker_thread_current_sandbox = sandbox;
	}
}
