#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <stdlib.h>
#include <threads.h>

#include "current_sandbox.h"
#include "local_completion_queue.h"
#include "local_runqueue.h"
#include "local_runqueue_list.h"
#include "local_runqueue_minheap.h"
#include "panic.h"
#include "runtime.h"
#include "scheduler.h"
#include "worker_thread.h"
#include "priority_queue.h"

/***************************
 * Worker Thread State     *
 **************************/

/* context of the runtime thread before running sandboxes or to resume its "main". */
thread_local struct arch_context worker_thread_base_context;

thread_local int worker_thread_epoll_file_descriptor;

/* Used to index into global arguments and deadlines arrays */
thread_local int worker_thread_idx;

/* Used to track tenants' timeouts */
thread_local struct priority_queue *worker_thread_timeout_queue;

/* Used to track worker's dbf */
thread_local struct dbf *worker_dbf;
thread_local struct sandbox_metadata *sandbox_meta;


/***********************
 * Worker Thread Logic *
 **********************/

/**
 * The entry function for sandbox worker threads
 * Initializes thread-local state, unmasks signals, sets up epoll loop and
 * @param argument - argument provided by pthread API. We set to -1 on error
 */
void *
worker_thread_main(void *argument)
{
	/* Set base context as running */
	worker_thread_base_context.variant = ARCH_CONTEXT_VARIANT_RUNNING;

	/* Index was passed via argument */
	worker_thread_idx = *(int *)argument;

	/* Set my priority */
	// runtime_set_pthread_prio(pthread_self(), 2);
	pthread_setschedprio(pthread_self(), -20);

	scheduler_runqueue_initialize();

	/* Initialize Completion Queue */
	local_completion_queue_initialize();

	/* To make sure global dbf reads out the max deadline in the system */
	sleep(1);

	if (scheduler == SCHEDULER_MTDS) {
		worker_thread_timeout_queue = priority_queue_initialize(RUNTIME_RUNQUEUE_SIZE, false,
		                                                        module_timeout_get_priority, NULL,
		                                                        NULL); ///// TODO: Change NULL!
	} else if (scheduler == SCHEDULER_MTDBF) {
		/* Initialize worker's dbf data structure */
		worker_dbf = dbf_initialize(1, 100, worker_thread_idx);
		worker_dbf = dbf_grow(worker_dbf, global_dbf->max_relative_deadline);
		// printf("WORKER ");
		// dbf_print(worker_dbf);
		sandbox_meta         = malloc(sizeof(struct sandbox_metadata));
		sandbox_meta->module = NULL;
	}

	/* Initialize epoll */
	worker_thread_epoll_file_descriptor = epoll_create1(0);
	if (unlikely(worker_thread_epoll_file_descriptor < 0)) panic_err();

	software_interrupt_unmask_signal(SIGFPE);
	software_interrupt_unmask_signal(SIGSEGV);

	/* Unmask signals, unless the runtime has disabled preemption */
	if (runtime_preemption_enabled) {
		software_interrupt_unmask_signal(SIGALRM);
		software_interrupt_unmask_signal(SIGUSR1);
	}

	scheduler_idle_loop();

	panic("Worker Thread unexpectedly completed idle loop.");
}
