#include <stdint.h>
#include <unistd.h>

#include "admissions_control.h"
#include "traffic_control.h"
#include "arch/getcycles.h"
#include "client_socket.h"
#include "global_request_scheduler.h"
#include "generic_thread.h"
#include "listener_thread.h"
#include "runtime.h"
#include "sandbox_functions.h"
#include "ck_ring.h"
#include "sandbox_perf_log.h"
#include "priority_queue.h"
#include "module_database.h"

/*
 * Descriptor of the epoll instance used to monitor the socket descriptors of registered
 * serverless modules. The listener cores listens for incoming client requests through this.
 */
int listener_thread_epoll_file_descriptor;

struct comm_with_worker *comm_from_workers;
struct comm_with_worker *comm_from_workers_extra;
struct comm_with_worker *comm_to_workers;

pthread_t     listener_thread_id;
extern lock_t global_lock;

extern struct priority_queue *LocalQueues[18]; // TODO temp

/**
 * Initializes the listener thread, pinned to core 0, and starts to listen for requests
 */
void
listener_thread_initialize(void)
{
	printf("Starting listener thread\n");

	comm_from_workers       = calloc(runtime_worker_threads_count, sizeof(struct comm_with_worker));
	comm_from_workers_extra = calloc(runtime_worker_threads_count, sizeof(struct comm_with_worker));
	comm_to_workers         = calloc(runtime_worker_threads_count, sizeof(struct comm_with_worker));
	comm_from_workers_init(comm_from_workers);
	comm_from_workers_init(comm_from_workers_extra);
	comm_to_workers_init(comm_to_workers);

	cpu_set_t cs;

	CPU_ZERO(&cs);
	CPU_SET(LISTENER_THREAD_CORE_ID, &cs);

	/* Setup epoll */
	listener_thread_epoll_file_descriptor = epoll_create1(0);
	assert(listener_thread_epoll_file_descriptor >= 0);

	int ret = pthread_create(&listener_thread_id, NULL, listener_thread_main, NULL);
	assert(ret == 0);
	ret = pthread_setaffinity_np(listener_thread_id, sizeof(cpu_set_t), &cs);
	assert(ret == 0);
	ret = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cs);
	assert(ret == 0);

	printf("\tListener core thread: %lx\n", listener_thread_id);
}

/**
 * @brief Registers a serverless module on the listener thread's epoll descriptor
 **/
int
listener_thread_register_module(struct module *mod)
{
	assert(mod != NULL);
	if (unlikely(listener_thread_epoll_file_descriptor == 0)) {
		panic("Attempting to register a module before listener thread initialization");
	}

	int                rc = 0;
	struct epoll_event accept_evt;
	accept_evt.data.ptr = (void *)mod;
	accept_evt.events   = EPOLLIN;
	rc = epoll_ctl(listener_thread_epoll_file_descriptor, EPOLL_CTL_ADD, mod->socket_descriptor, &accept_evt);

	return rc;
}

static void
check_messages_from_workers()
{
#ifdef TRAFFIC_CONTROL
	assert(comm_from_workers);
	// assert(comm_from_workers_extra);
	assert(comm_to_workers);

	for (int worker_idx = 0; worker_idx < runtime_worker_threads_count; worker_idx++) {
		struct message           new_message = { 0 };
		struct comm_with_worker *cfw         = &comm_from_workers[worker_idx];
		struct comm_with_worker *ctw         = &comm_to_workers[worker_idx];
		assert(cfw);
		assert(ctw);
		assert(cfw->worker_idx == worker_idx);
		assert(ctw->worker_idx == worker_idx);
		// printf ("[LISTENER] Worker#%d CFW Ring size: %u\n", worker_idx, ck_ring_size(&cfw->worker_ring));
		assert(ck_ring_size(&cfw->worker_ring) < LISTENER_THREAD_RING_SIZE);
		assert(ck_ring_size(&ctw->worker_ring) < LISTENER_THREAD_RING_SIZE);

		while (ck_ring_dequeue_spsc_message(&cfw->worker_ring, cfw->worker_ring_buffer, &new_message)) {
			assert(new_message.sender_worker_idx == worker_idx);
			assert(new_message.sandbox);
			assert(new_message.module);
			assert(new_message.sandbox_meta);

			// assert(new_message.dbf_update_mode == DBF_REDUCE_EXISTING_DEMAND);

			const uint64_t           now          = new_message.last_extra_demand_timestamp;
			struct sandbox_metadata *sandbox_meta = new_message.sandbox_meta;
			assert(new_message.sandbox == sandbox_meta->sandbox_shadow);
			assert(new_message.module == sandbox_meta->module);
			assert(new_message.sandbox_id == sandbox_meta->id);
			assert(new_message.absolute_deadline == sandbox_meta->absolute_deadline);
			assert(new_message.sender_worker_idx == sandbox_meta->owned_worker_idx);
			sandbox_meta->exceeded_estimation = new_message.exceeded_estimation;

			if (!sandbox_meta->terminated) {
				sandbox_meta->remaining_execution = new_message.remaining_execution;
			} else if (sandbox_meta->remaining_execution >= runtime_quantum/2) {
				dbf_try_update_demand(new_message.module->module_dbf, now,
				                      new_message.module->relative_deadline,
				                      new_message.absolute_deadline, sandbox_meta->remaining_execution,
				                      DBF_FORCE_ADD_NEW_SANDBOX_DEMAND, &new_message);

				dbf_try_update_demand(global_dbf, now, new_message.module->relative_deadline,
				                      new_message.absolute_deadline, sandbox_meta->remaining_execution,
				                      DBF_FORCE_ADD_NEW_SANDBOX_DEMAND, &new_message);

				sandbox_meta->remaining_execution = 0;
			}

			if (new_message.dbf_update_mode == DBF_REDUCE_EXISTING_DEMAND) {
				if (!new_message.exceeded_estimation)
					assert(
					  now == new_message.absolute_deadline - new_message.module->relative_deadline);

				// printf("New message from Worker %d. Its contents: \n\
			// module: %s \n\
			// dbf_mode: %u \n\
			// abs_deadline: %lu \n\
			// adjustment: %ld \n",
				//        worker_idx, new_message.module->name, new_message.dbf_update_mode,
				//        new_message.absolute_deadline, new_message.adjustment);
				// if (new_message.sandbox) {
				// 	printf("Sandbox details:\n");
				// 	printf("\
			// sandbox->id: %lu \n\
			// sandbox->pq_idx_in_runqueue: %lu \n",
				// 	       new_message.sandbox->id, new_message.sandbox->pq_idx_in_runqueue);
				// }
				// if (!sandbox_meta->exceeded_estimation && !sandbox_meta->terminated) {
				dbf_try_update_demand(new_message.module->module_dbf, now,
				                      new_message.module->relative_deadline,
				                      new_message.absolute_deadline, new_message.adjustment,
				                      DBF_REDUCE_EXISTING_DEMAND, &new_message);

				dbf_try_update_demand(global_dbf, now, new_message.module->relative_deadline,
				                      new_message.absolute_deadline, new_message.adjustment,
				                      DBF_REDUCE_EXISTING_DEMAND, &new_message);
				// }

				if (new_message.state == SANDBOX_RETURNED || new_message.state == SANDBOX_ERROR) {
					if (!sandbox_meta->terminated) {
						LOCK_LOCK(&global_lock);
						if (!sandbox_meta->terminated) {
							priority_queue_delete_by_idx_nolock(new_message.module
							                                      ->local_sandbox_metas,
							                                    sandbox_meta,
							                                    sandbox_meta
							                                      ->pq_idx_in_module_queue);
							sandbox_meta->terminated = true;
						}
						LOCK_UNLOCK(&global_lock);
					}
					free(sandbox_meta);
				}
			} else if (new_message.dbf_update_mode == DBF_CHECK_EXISTING_SANDBOX_EXTRA_DEMAND) {
				assert(new_message.exceeded_estimation);
				assert(new_message.sandbox);
				assert(now < new_message.absolute_deadline);
				// assert (new_message.absolute_deadline - now >= new_message.remaining_execution);

				// printf("Worker %d sent extra demand request for sandbox #%lu!\n", worker_idx,
				// new_message.sandbox_id);

				// if (sandbox_meta->terminated
				//     || !sandbox_refs[new_message.sandbox_id % RUNTIME_MAX_ALIVE_SANDBOXES]) {
				// 	// printf("Sandbox %lu already unavailable.\n", new_message.sandbox_id);
				// 	if (!sandbox_meta->terminated) {
				// 		LOCK_LOCK(&global_lock);
				// 		priority_queue_delete_by_idx_nolock(new_message.module
				// 		                                      ->local_sandbox_metas,
				// 		                                    sandbox_meta,
				// 		                                    sandbox_meta
				// 		                                      ->pq_idx_in_module_queue);
				// 		sandbox_meta->terminated = true;
				// 		LOCK_UNLOCK(&global_lock);
				// 	}
				// 	continue;
				// }

				// if (sandbox_meta->terminated) {
				// 	printf("Sandbox meta %lu already terminated.\n", sandbox_meta->id);
				// 	// continue;
				// }

				bool module_can_admit = dbf_try_update_demand(new_message.module->module_dbf, now,
				                                              new_message.module->relative_deadline,
				                                              new_message.absolute_deadline,
				                                              new_message.adjustment,
				                                              DBF_FORCE_ADD_NEW_SANDBOX_DEMAND,
				                                              &new_message);

				bool global_can_admit = dbf_try_update_demand(global_dbf, now,
				                                              new_message.module->relative_deadline,
				                                              new_message.absolute_deadline,
				                                              new_message.adjustment,
				                                              DBF_FORCE_ADD_NEW_SANDBOX_DEMAND,
				                                              &new_message);

				// if (now >= new_message.absolute_deadline || new_message.absolute_deadline - now <
				// new_message.remaining_execution) { 	new_message.extra_demand_request_approved =
				// false; } else

				if (module_can_admit && global_can_admit) {
					/* Case #1: Both the tenant and overall system is under utlized. So, approve
					 * extra demand. */
					// printf("Approved extra demand for worker %d.\n", worker_idx);
					// new_message.extra_demand_request_approved = true;
				} else if (!module_can_admit && global_can_admit) {
					/* Case #2: Tenant is over utilized, but overall system is under utilized. So,
					 * approve extra demand for work-conservation purposes. */
					// printf("Approved extra demand for worker %d for work conservation\n",
					// worker_idx);
					// new_message.extra_demand_request_approved = true;
				} else if (module_can_admit && !global_can_admit) {
					/* Case #3: Tenant is under utilized, but overall system is over utilized. So,
					 * try to shed work and then approve extra demand if possible. */
					// new_message.extra_demand_request_approved = true;

					// while (!global_can_admit) {
						int rc = global_request_scheduler_mtdbf_shed_work(NULL, now,
						                                                  new_message
						                                                    .absolute_deadline);
						if (rc != 0) {
							/* This case is not desirable, where there is no bad module
							 * requests left in the global queue, so we have deny the
							 * guaranteed tenant job. */
							// assert(0);
							// new_message.extra_demand_request_approved = false;
							printf("Just approved extra demand, since guaranteed\n");
							// break;
						}

						// new_message.if_case = 334;
						// global_can_admit =
						//   dbf_try_update_demand(global_dbf, now,
						//                         new_message.module->relative_deadline,
						//                         new_message.absolute_deadline,
						//                         new_message.adjustment,
						//                         DBF_CHECK_EXISTING_SANDBOX_EXTRA_DEMAND,
						//                         &new_message);
					// }
				} else {
					/* Case #4: Do not approve extra demand. */
					// printf("Denied extra demand for worker %d.\n", worker_idx);
					// new_message.extra_demand_request_approved = false;
					if (!ck_ring_enqueue_spsc_message(&ctw->worker_ring, ctw->worker_ring_buffer,
					                                  &new_message)) {
						panic("Ring The buffer was full and the enqueue "
						      "operation has failed.!")
					}
				}

				// if (!ck_ring_enqueue_spsc_message(&ctw->worker_ring, ctw->worker_ring_buffer,
				//                                   &new_message)) {
				// 	panic("Ring The buffer was full and the enqueue "
				// 	      "operation has failed.!")
				// }
			}
			memset(&new_message, 0, sizeof(new_message));
		}
	}


	// for (int worker_idx = 0; worker_idx < runtime_worker_threads_count; worker_idx++) {
	// 	struct message           new_message = { 0 };
	// 	struct comm_with_worker *cfw_extra   = &comm_from_workers_extra[worker_idx];
	// 	struct comm_with_worker *ctw         = &comm_to_workers[worker_idx];
	// 	assert(cfw_extra);
	// 	assert(ctw);
	// 	assert(cfw_extra->worker_idx == worker_idx);
	// 	assert(ctw->worker_idx == worker_idx);
	// 	// printf ("[LISTENER] Worker#%d CFW_EXTRA Ring size: %u\n", worker_idx,
	// 	// ck_ring_size(&cfw_extra->worker_ring)); printf ("[LISTENER] Worker#%d CTW Ring size: %u\n",
	// 	// worker_idx, ck_ring_size(&ctw->worker_ring));
	// 	assert(ck_ring_size(&cfw_extra->worker_ring) < LISTENER_THREAD_RING_SIZE);
	// 	assert(ck_ring_size(&ctw->worker_ring) < LISTENER_THREAD_RING_SIZE);
	// 	// uint64_t now = __getcycles();
	// 	while (
	// 	  ck_ring_dequeue_spsc_message(&cfw_extra->worker_ring, cfw_extra->worker_ring_buffer, &new_message)) {
	// 		assert(new_message.module != NULL);
	// 		// uint64_t now = __getcycles();
	// 		const uint64_t now = new_message.last_extra_demand_timestamp;
	// 		// printf("New message from Worker %d. Its contents: \n\
	// 		// module: %s \n\
	// 		// dbf_mode: %u \n\
	// 		// abs_deadline: %lu \n\
	// 		// adjustment: %ld \n",
	// 		//        worker_idx, new_message.module->name, new_message.dbf_update_mode,
	// 		//        new_message.absolute_deadline, new_message.adjustment);
	// 		// if (new_message.sandbox) {
	// 		// 	printf("Sandbox details:\n");
	// 		// 	printf("\
	// 		// sandbox->id: %lu \n\
	// 		// sandbox->pq_idx_in_runqueue: %lu \n",
	// 		// 	       new_message.sandbox->id, new_message.sandbox->pq_idx_in_runqueue);
	// 		// }
	// 		assert(new_message.dbf_update_mode == DBF_CHECK_EXISTING_SANDBOX_EXTRA_DEMAND);
	// 		// 	printf("Sandbox details:\n");
	// 		// 	printf("\
	// 		// sandbox->id: %lu \n\
	// 		// sandbox->owned_worker: %d \n\
	// 		// sandbox->pq_idx_in_runqueue: %lu \n",
	// 		// 	       new_message.sandbox->id, new_message.sandbox->owned_worker_idx,
	// 		// new_message.sandbox->pq_idx_in_runqueue);
	// 		// printf("Worker %d sent extra_demand request for sandbox #%lu!\n", worker_idx,
	// 		// new_message.sandbox_id);
	// 		assert(new_message.sandbox);
	// 		// if (!sandbox_refs[new_message.sandbox_id % RUNTIME_MAX_ALIVE_SANDBOXES]) {
	// 		// 	printf("Sandbox %lu already unavailable.\n", new_message.sandbox_id);
	// 		// 	continue;
	// 		// }
	// 		bool module_can_admit = dbf_try_update_demand(new_message.module->module_dbf, now,
	// 		                                              new_message.module->relative_deadline,
	// 		                                              new_message.absolute_deadline,
	// 		                                              new_message.adjustment,
	// 		                                              DBF_FORCE_ADD_NEW_SANDBOX_DEMAND, &new_message);
	// 		bool global_can_admit = dbf_try_update_demand(global_dbf, now,
	// 		                                              new_message.module->relative_deadline,
	// 		                                              new_message.absolute_deadline,
	// 		                                              new_message.adjustment,
	// 		                                              DBF_FORCE_ADD_NEW_SANDBOX_DEMAND, &new_message);
	// 		// if (now >= new_message.absolute_deadline || new_message.absolute_deadline - now <
	// 		// new_message.remaining_execution) { 	new_message.extra_demand_request_approved = false; }
	// 		// else
	// 		assert(now < new_message.absolute_deadline);
	// 		// assert (new_message.absolute_deadline - now >= new_message.remaining_execution);
	// 		if (module_can_admit && global_can_admit) {
	// 			/* Case #1: Both the tenant and overall system is under utlized. So, approve extra
	// 			 * demand. */
	// 			// printf("Approved extra demand for worker %d.\n", worker_idx);
	// 			new_message.extra_demand_request_approved = true;
	// 		} else if (!module_can_admit && global_can_admit) {
	// 			/* Case #2: Tenant is over utilized, but overall system is under utilized. So,
	// 			 * approve extra demand for work-conservation purposes. */
	// 			// printf("Approved extra demand for worker %d for work conservation.\n", worker_idx);
	// 			new_message.if_case = 222;
	// 			// dbf_try_update_demand(new_message.module->module_dbf, now,
	// 			//                       new_message.module->relative_deadline,
	// 			//                       new_message.absolute_deadline, new_message.adjustment,
	// 			//                       DBF_FORCE_ADD_NEW_SANDBOX_DEMAND, &new_message);
	// 			new_message.extra_demand_request_approved = true;
	// 		} else if (module_can_admit && !global_can_admit) {
	// 			/* Case #3: Tenant is under utilized, but overall system is over utilized. So,
	// 			 * try to shed work and then approve extra demand if possible. */
	// 			new_message.extra_demand_request_approved = true;
	// 			while (!global_can_admit) {
	// 				int rc =
	// 				  global_request_scheduler_mtdbf_shed_work(NULL, now,
	// 				                                           new_message.absolute_deadline);
	// 				if (rc != 0) {
	// 					/* This case is not desirable, where there is no bad module
	// 					 * requests left in the global queue, so we have deny the
	// 					 * guaranteed tenant job. */
	// 					// assert(0);
	// 					new_message.if_case = 333;
	// 					// dbf_try_update_demand(new_message.module->module_dbf, now,
	// 					//                       new_message.module->relative_deadline,
	// 					//                       new_message.absolute_deadline,
	// 					//                       new_message.adjustment,
	// 					//                       DBF_REDUCE_EXISTING_DEMAND,
	// 					//                       &new_message);
	// 					// printf("Sorry guaranteed sandbox #%lu in worker %d, Nothing
	// 					// to "
	// 					//        "shed in "
	// 					//        "global queue.\n",
	// 					//        new_message.sandbox_id, worker_idx);
	// 					// printf("Guaranteed Sandbox's state was: %u\n",
	// 					//        new_message.state);
	// 					// if (!ck_ring_enqueue_spsc_message(&ctw->worker_ring,
	// 					//                                   ctw->worker_ring_buffer,
	// 					//                                   &new_message)) {
	// 					// 	panic("Ring The buffer was full and the enqueue "
	// 					// 	      "operation has failed.!")
	// 					// }
	// 					// 		printf("LISTENER:\nMODULE DBF DEMANDS during case3:\n");
	// 					// dbf_print(new_message.module->module_dbf);
	// 					// printf("\nBAD MODULE DBF DEMANDS during case3:\n");
	// 					// dbf_print(m->module_dbf);
	// 					// printf("\nGLOBAL DBF DEMANDS during case3:\n");
	// 					// dbf_print(global_dbf);
	// 					// exit(-1);
	// 					new_message.extra_demand_request_approved = false;
	// 					break;
	// 				}
	// 				new_message.if_case = 334;
	// 				global_can_admit =
	// 				  dbf_try_update_demand(global_dbf, now, new_message.module->relative_deadline,
	// 				                        new_message.absolute_deadline, new_message.adjustment,
	// 				                        DBF_CHECK_EXISTING_SANDBOX_EXTRA_DEMAND, &new_message);
	// 			}
	// 		} else {
	// 			/* Case #4: Do not approve extra demand. */
	// 			// printf("Denied extra demand for worker %d.\n", worker_idx);
	// 			// if (!ck_ring_enqueue_spsc_message(&ctw->worker_ring, ctw->worker_ring_buffer,
	// 			//                                   &new_message)) {
	// 			// 	panic("Ring The buffer was full and the enqueue operation has failed.!")
	// 			// }
	// 			new_message.extra_demand_request_approved = false;
	// 		}
	// 		if (!ck_ring_enqueue_spsc_message(&ctw->worker_ring, ctw->worker_ring_buffer, &new_message)) {
	// 			panic("Ring The buffer was full and the enqueue "
	// 			      "operation has failed.!")
	// 		}
	// 		memset(&new_message, 0, sizeof(new_message));
	// 	}
	// }

#endif
}

/**
 * @brief Execution Loop of the listener core, io_handles HTTP requests, allocates sandbox request objects, and
 * pushes the sandbox object to the global dequeue
 * @param dummy data pointer provided by pthreads API. Unused in this function
 * @return NULL
 *
 * Used Globals:
 * listener_thread_epoll_file_descriptor - the epoll file descriptor
 *
 */
noreturn void *
listener_thread_main(void *dummy)
{
	struct epoll_event epoll_events[RUNTIME_MAX_EPOLL_EVENTS];

	generic_thread_initialize();

	/* Set my priority */
	// runtime_set_pthread_prio(pthread_self(), 2);
	pthread_setschedprio(pthread_self(), -20);

#ifdef TRAFFIC_CONTROL
	const int epoll_timeout = 0;
#else
	const int epoll_timeout = -1;
#endif

	while (true) {
		/*
		 * Block indefinitely on the epoll file descriptor, waiting on up to a max number of events
		 * TODO: Is RUNTIME_MAX_EPOLL_EVENTS actually limited to the max number of modules?
		 */
		int descriptor_count = epoll_wait(listener_thread_epoll_file_descriptor, epoll_events,
		                                  RUNTIME_MAX_EPOLL_EVENTS, epoll_timeout);

		// global_request_scheduler_mtdbf_clear_dead_requests(); // LOCK overhead is crazy with this!
		check_messages_from_workers();

		if (descriptor_count == 0) continue;
		if (descriptor_count < 0) {
			if (errno == EINTR) continue;

			panic("epoll_wait: %s", strerror(errno));
		}
		assert(descriptor_count > 0);

		const uint64_t request_arrival_timestamp = __getcycles();
		for (int i = 0; i < descriptor_count; i++) {
			/* Check Event to determine if epoll returned an error */
			if ((epoll_events[i].events & EPOLLERR) == EPOLLERR) {
				int       error  = 0;
				socklen_t errlen = sizeof(error);
				if (getsockopt(epoll_events[i].data.fd, SOL_SOCKET, SO_ERROR, (void *)&error, &errlen)
				    == 0) {
					panic("epoll_wait: %s\n", strerror(error));
				}
				panic("epoll_wait");
			};

			/* Assumption: We have only registered EPOLLIN events, so we should see no others here */
			assert((epoll_events[i].events & EPOLLIN) == EPOLLIN);

			/* Unpack module from epoll event */
			struct module *module = (struct module *)epoll_events[i].data.ptr;
			assert(module);

			if (module->port == 11111) {
				printf("Hello from Admin!\n");
				for (int i = 0; i < module_database_count; i++) {
					struct module *m = module_database[i];
					if (m->port == 11111 || m->port == 55555) continue;
					printf("\nMODULE: %s DBF DEMANDS:\n", m->name);
					dbf_print(m->module_dbf);
				}
				printf("\nGLOBAL DBF DEMANDS:\n");
				dbf_print(global_dbf);
			}

			if (module->port == 55555) {
				printf("Terminating SLEdge!\n");
				runtime_cleanup();
				exit(0);
			}

			/*
			 * I don't think we're responsible to cleanup epoll events, but clearing to trigger
			 * the assertion just in case.
			 */
			epoll_events[i].data.ptr = NULL;

			/* Accept Client Request as a nonblocking socket, saving address information */
			struct sockaddr_in client_address;
			socklen_t          address_length = sizeof(client_address);

			/*
			 * Accept as many requests as possible, terminating when we would have blocked
			 * This inner loop is used in case there are more datagrams than epoll events for some
			 * reason
			 */
			while (true) {
				int client_socket = accept4(module->socket_descriptor,
				                            (struct sockaddr *)&client_address, &address_length,
				                            SOCK_NONBLOCK);
				if (unlikely(client_socket < 0)) {
					if (errno == EWOULDBLOCK || errno == EAGAIN) break;

					panic("accept4: %s", strerror(errno));
				}

				/* We should never have accepted on fd 0, 1, or 2 */
				assert(client_socket != STDIN_FILENO);
				assert(client_socket != STDOUT_FILENO);
				assert(client_socket != STDERR_FILENO);

				/*
				 * According to accept(2), it is possible that the the sockaddr structure
				 * client_address may be too small, resulting in data being truncated to fit.
				 * The accept call mutates the size value to indicate that this is the case.
				 */
				if (address_length > sizeof(client_address)) {
					debuglog("Client address %s truncated because buffer was too small\n",
					         module->name);
				}

				http_total_increment_request();


				const uint64_t absolute_deadline = request_arrival_timestamp
				                                   + module->relative_deadline;
				const uint64_t estimated_execution = module->estimated_exec_info.estimated_execution;
				int            work_admitted       = 1;

#if defined ADMISSIONS_CONTROL
				/*
				 * Perform admissions control.
				 * If 0, workload was rejected, so close with 429 "Too Many Requests" and continue
				 * TODO: Consider providing a Retry-After header
				 */
				work_admitted = admissions_control_decide(module->admissions_info.estimate);
#elif defined TRAFFIC_CONTROL
				work_admitted = traffic_control_decide(module, absolute_deadline, estimated_execution);
#endif
				if (work_admitted != 1) {
					client_socket_send_oneshot(client_socket, http_header_build(429),
					                           http_header_len(429));
					if (unlikely(close(client_socket) < 0))
						debuglog("Error closing client socket - %s", strerror(errno));

					sandbox_perf_log_print_denied_entry(module, work_admitted);
					continue;
				}

				/* Allocate a Sandbox */
				struct sandbox *sandbox = sandbox_alloc(module, client_socket,
				                                        (const struct sockaddr *)&client_address,
				                                        request_arrival_timestamp, estimated_execution);
				if (unlikely(sandbox == NULL)) {
					client_socket_send_oneshot(sandbox->client_socket_descriptor,
					                           http_header_build(503), http_header_len(503));
					client_socket_close(sandbox->client_socket_descriptor,
					                    &sandbox->client_address);
				}
				struct sandbox *temp = NULL;
				/* If the global request scheduler is full, return a 429 to the client */
				temp = global_request_scheduler_add(sandbox);
				if (unlikely(temp == NULL)) {
					client_socket_send_oneshot(sandbox->client_socket_descriptor,
					                           http_header_build(429), http_header_len(429));
					client_socket_close(sandbox->client_socket_descriptor,
					                    &sandbox->client_address);

					sandbox_perf_log_print_denied_entry(module,
					                                    999); // just to distinguish from above
					sandbox_free(sandbox);
					// TODO: Free sandbox? ALready added by Sean in the new master
				}

			} /* while true */
		}         /* for loop */
		generic_thread_dump_lock_overhead();
	} /* while true */

	panic("Listener thread unexpectedly broke loop\n");
}
