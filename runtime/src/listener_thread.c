#include <stdint.h>
#include <unistd.h>

#include "arch/getcycles.h"
#include "global_request_scheduler.h"
#include "listener_thread.h"
#include "metrics_server.h"
#include "module.h"
#include "runtime.h"
#include "sandbox_functions.h"
#include "tcp_session.h"
#include "tenant.h"
#include "tenant_functions.h"
#include "http_session_perf_log.h"
#include "sandbox_set_as_runnable.h"

/** SLEDGE GENERATOR **/
#include <stdlib.h>
#include <math.h>
#include "local_runqueue.h"
#define NB_WORKER 10
int nb_generator = 0;
extern thread_local bool is_generator;
thread_local uint64_t nb_sandbox = 0;
thread_local uint64_t sandbox_added = 0;
thread_local uint64_t sandbox_lost = 0;
struct priority_queue* worker_queues[1024];
struct http_session *g_session[1024];
thread_local http_session *local_session = NULL;
bool change[1024];
int rate[1024];
int input[1024];
pthread_t generator[1024];
cpu_set_t cs[1024];
int first_runtime_generator = 2;
/** SLEDGE GENERATOR **/

extern thread_local int thread_id;
extern bool first_request_comming;
time_t t_start;
extern uint32_t runtime_worker_threads_count;
thread_local int rr_index = 0;
static void listener_thread_unregister_http_session(struct http_session *http);
static void panic_on_epoll_error(struct epoll_event *evt);

static void on_client_socket_epoll_event(struct epoll_event *evt);
static void on_tenant_socket_epoll_event(struct epoll_event *evt);
static void on_client_request_arrival(int client_socket, const struct sockaddr *client_address, struct tenant *tenant);
static void on_client_request_receiving(struct http_session *session);
static void on_client_request_received(struct http_session *session);
static void on_client_response_header_sending(struct http_session *session);
static void on_client_response_body_sending(struct http_session *session);
static void on_client_response_sent(struct http_session *session);

struct auto_buf yves_request;

/*
 * Descriptor of the epoll instance used to monitor the socket descriptors of registered
 * serverless modules. The listener cores listens for incoming client requests through this.
 */
int listener_thread_epoll_file_descriptor;

pthread_t listener_thread_id;

/**
 * Initializes the listener thread, pinned to core 0, and starts to listen for requests
 */
void
listener_thread_initialize(void)
{
	printf("Starting listener thread\n");
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
 * @brief Registers a serverless tenant on the listener thread's epoll descriptor
 **/
void
listener_thread_register_http_session(struct http_session *http)
{
	assert(http != NULL);

	if (unlikely(listener_thread_epoll_file_descriptor == 0)) {
		panic("Attempting to register an http session before listener thread initialization");
	}

	int                rc = 0;
	struct epoll_event accept_evt;
	accept_evt.data.ptr = (void *)http;

	switch (http->state) {
	case HTTP_SESSION_RECEIVING_REQUEST:
		accept_evt.events = EPOLLIN;
		http->state       = HTTP_SESSION_RECEIVE_REQUEST_BLOCKED;
		break;
	case HTTP_SESSION_SENDING_RESPONSE_HEADER:
		accept_evt.events = EPOLLOUT;
		http->state       = HTTP_SESSION_SEND_RESPONSE_HEADER_BLOCKED;
		break;
	case HTTP_SESSION_SENDING_RESPONSE_BODY:
		accept_evt.events = EPOLLOUT;
		http->state       = HTTP_SESSION_SEND_RESPONSE_BODY_BLOCKED;
		break;
	default:
		panic("Invalid HTTP Session State: %d\n", http->state);
	}

	rc = epoll_ctl(listener_thread_epoll_file_descriptor, EPOLL_CTL_ADD, http->socket, &accept_evt);
	if (rc != 0) { panic("Failed to add http session to listener thread epoll\n"); }
}

/**
 * @brief Registers a serverless tenant on the listener thread's epoll descriptor
 **/
static void
listener_thread_unregister_http_session(struct http_session *http)
{
	assert(http != NULL);

	if (unlikely(listener_thread_epoll_file_descriptor == 0)) {
		panic("Attempting to unregister an http session before listener thread initialization");
	}

	int rc = epoll_ctl(listener_thread_epoll_file_descriptor, EPOLL_CTL_DEL, http->socket, NULL);
	if (rc != 0) { panic("Failed to remove http session from listener thread epoll\n"); }
}

/**
 * @brief Registers a serverless tenant on the listener thread's epoll descriptor
 * Assumption: We never have to unregister a tenant
 **/
int
listener_thread_register_tenant(struct tenant *tenant)
{
	assert(tenant != NULL);
	if (unlikely(listener_thread_epoll_file_descriptor == 0)) {
		panic("Attempting to register a tenant before listener thread initialization");
	}

	int                rc = 0;
	struct epoll_event accept_evt;
	accept_evt.data.ptr = (void *)tenant;
	accept_evt.events   = EPOLLIN;
	rc = epoll_ctl(listener_thread_epoll_file_descriptor, EPOLL_CTL_ADD, tenant->tcp_server.socket_descriptor,
	               &accept_evt);

	return rc;
}

int
listener_thread_register_metrics_server()
{
	if (unlikely(listener_thread_epoll_file_descriptor == 0)) {
		panic("Attempting to register metrics_server before listener thread initialization");
	}

	int                rc = 0;
	struct epoll_event accept_evt;
	accept_evt.data.ptr = (void *)&metrics_server;
	accept_evt.events   = EPOLLIN;
	rc = epoll_ctl(listener_thread_epoll_file_descriptor, EPOLL_CTL_ADD, metrics_server.tcp.socket_descriptor,
	               &accept_evt);

	return rc;
}

static void
panic_on_epoll_error(struct epoll_event *evt)
{
	/* Check Event to determine if epoll returned an error */
	if ((evt->events & EPOLLERR) == EPOLLERR) {
		int       error  = 0;
		socklen_t errlen = sizeof(error);
		if (getsockopt(evt->data.fd, SOL_SOCKET, SO_ERROR, (void *)&error, &errlen) == 0) {
			panic("epoll_wait: %s\n", strerror(error));
		}
		debuglog("epoll_error: Most likely client disconnected. Closing session.");
	}
}

static void
on_client_request_arrival(int client_socket, const struct sockaddr *client_address, struct tenant *tenant)
{
	uint64_t request_arrival_timestamp = __getcycles();

	http_total_increment_request();

	/* Allocate HTTP Session */
	struct http_session *session = http_session_alloc(client_socket, (const struct sockaddr *)&client_address,
	                                                  tenant, request_arrival_timestamp);
	if (likely(session != NULL)) {
		on_client_request_receiving(session);
		return;
	} else {
		/* Failed to allocate memory */
		debuglog("Failed to allocate http session\n");
		session->state = HTTP_SESSION_EXECUTION_COMPLETE;
		http_session_set_response_header(session, 500);
		on_client_response_header_sending(session);
		return;
	}
}

static void
on_client_request_receiving(struct http_session *session)
{
	/* Read HTTP request */
	int rc = http_session_receive_request(session, (void_star_cb)listener_thread_register_http_session);
	if (likely(rc == 0)) {
		on_client_request_received(session);
		return;
	} else if (unlikely(rc == -EAGAIN)) {
		/* session blocked and registered to epoll so continue to next handle */
		return;
	} else if (rc < 0) {
		debuglog("Failed to receive or parse request\n");
		session->state = HTTP_SESSION_EXECUTION_COMPLETE;
		http_session_set_response_header(session, 400);
		on_client_response_header_sending(session);
		return;
	}

	assert(0);
}

double ran_expo(double lambda){
    double u;

    u = rand() / (RAND_MAX + 1.0);

    return -log(1- u) / lambda;
}
	
noreturn void*
generator_main(int idx)
{       
	uint64_t begin, end;
	is_generator = true;
        printf("generator coucou %d starts\n", idx);
        pthread_setschedprio(pthread_self(), -20);
	local_session = NULL; //http_session_alloc(g_session[idx]->socket, (const struct sockaddr *)&(g_session[idx]->client_address),
  //                                                    g_session[idx]->tenant, g_session[idx]->request_arrival_timestamp);
	software_interrupt_unmask_signal(SIGINT);
	int rr_index = 0;
	while (true) {
		if (change[idx]) {
			if (local_session != NULL)
				http_session_free(local_session);
			local_session = http_session_alloc(g_session[idx]->socket, (const struct sockaddr *)&(g_session[idx]->client_address),
                                                          g_session[idx]->tenant, g_session[idx]->request_arrival_timestamp);
			http_session_copy(local_session, g_session[idx]);	
			change[idx] = false;
		}
		struct sandbox *sandbox = sandbox_alloc(local_session->route->module, local_session, local_session->route, local_session->tenant, 1);
		nb_sandbox++;
		
		if (sandbox && global_request_scheduler_add(sandbox) == NULL) {
			sandbox_lost++;
			sandbox->http = NULL;
                        sandbox->state = SANDBOX_COMPLETE;
                        sandbox_free(sandbox);
		}else { 
			sandbox_added++;
		}
		int cycles = ran_expo(1.0/rate[idx]); 
		begin = __getcycles();
        	end = begin;
        	while(end - begin < cycles) {
                	end = __getcycles();
		}
		rr_index = (rr_index + 1) % runtime_worker_threads_count;
	}

}


struct route *route = NULL;
uint64_t work_admitted = 0;
static void
on_client_request_received(struct http_session *session)
{
	assert(session->state == HTTP_SESSION_RECEIVED_REQUEST);
	session->request_downloaded_timestamp = __getcycles();

	route = http_router_match_route(&session->tenant->router, session->http_request.full_url);
	if (route == NULL) {
		debuglog("Did not match any routes\n");
		session->state = HTTP_SESSION_EXECUTION_COMPLETE;
		http_session_set_response_header(session, 404);
		on_client_response_header_sending(session);
		return;
	}

	session->route = route;
	/*
	 * Perform admissions control.
	 * If 0, workload was rejected, so close with 429 "Too Many Requests" and continue
	 * TODO: Consider providing a Retry-After header
	 */
	work_admitted = admissions_control_decide(route->admissions_info.estimate);
	if (work_admitted == 0) {
		session->state = HTTP_SESSION_EXECUTION_COMPLETE;
		http_session_set_response_header(session, 429);
		on_client_response_header_sending(session);
		return;
	}

	session->state          = HTTP_SESSION_EXECUTING;

	
	session->state = HTTP_SESSION_EXECUTION_COMPLETE;

	bool new_thread = false;

	/* Request Pqrsing */
	char *req_body = strdup(session->http_request.body);
        int idx_gen = nb_generator;
	int tmp = atoi(strtok(req_body, " "));
	nb_generator++;
	rate[idx_gen] = atoi(strtok(NULL, " "));
	input[idx_gen] = atoi(strtok(NULL, " "));
	int shift;
	printf("idx %d rate %d input %d\n", idx_gen, rate[idx_gen], input[idx_gen]);

	/*HTTP Session set up */
       	if (g_session[idx_gen] == NULL) 
		new_thread = true;	
	if (g_session[idx_gen] != NULL)
		http_session_free(g_session[idx_gen]);
	g_session[idx_gen] = http_session_alloc(session->socket, (const struct sockaddr *)&(session->client_address),
                                                        session->tenant, session->request_arrival_timestamp);
	http_session_copy(g_session[idx_gen], session);
	
	/*HTTP Session Body update */
	if(rate[idx_gen] < 100000)
		shift = 6;
	else 
		shift = 7;

	if (idx_gen < 10)
		shift += 2;
	else
		shift += 3;

	g_session[idx_gen]->http_request.body += shift;
	g_session[idx_gen]->http_request.body_length -= shift;
	
	/* Response to CLient */
	http_session_set_response_header(session, 200);
	on_client_response_header_sending(session);
	for (int i = 0; i < 1024; i++) {
		if (g_session[i] == NULL) continue;
		printf("session %d: %s %s\n",i, g_session[i]->http_request.body, g_session[idx_gen]->route->module->path);
	}

	/* Generator Thread Creation */
	change[idx_gen] = true;
	if (new_thread) {
		CPU_ZERO(&cs[idx_gen]);
       		CPU_SET(first_runtime_generator, &cs[idx_gen]);
		first_runtime_generator++;
               	pthread_create(&generator[idx_gen], NULL, generator_main, idx_gen);
               	int pin_ret = pthread_setaffinity_np(generator[idx_gen], sizeof(cs[idx_gen]), &cs[idx_gen]);
	}
	if (nb_generator == 2)
		t_start = time(NULL);
}

static void
on_client_response_header_sending(struct http_session *session)
{
	int rc = http_session_send_response_header(session, (void_star_cb)listener_thread_register_http_session);
	if (likely(rc == 0)) {
		on_client_response_body_sending(session);
		return;
	} else if (unlikely(rc == -EAGAIN)) {
		/* session blocked and registered to epoll so continue to next handle */
		return;
	} else if (rc < 0) {
		http_session_close(session);
		http_session_free(session);
		return;
	}
}

static void
on_client_response_body_sending(struct http_session *session)
{
	/* Read HTTP request */
	int rc = http_session_send_response_body(session, (void_star_cb)listener_thread_register_http_session);
	if (likely(rc == 0)) {
		on_client_response_sent(session);
		return;
	} else if (unlikely(rc == -EAGAIN)) {
		/* session blocked and registered to epoll so continue to next handle */
		return;
	} else if (unlikely(rc < 0)) {
		http_session_close(session);
		http_session_free(session);
		return;
	}
}

static void
on_client_response_sent(struct http_session *session)
{
	assert(session->state = HTTP_SESSION_SENT_RESPONSE_BODY);

	/* Terminal State Logging for Http Session */
	session->response_sent_timestamp = __getcycles();
	http_session_perf_log_print_entry(session);

//	http_session_close(session);
//	http_session_free(session);
	return;
}

static void
on_tenant_socket_epoll_event(struct epoll_event *evt)
{
	assert((evt->events & EPOLLIN) == EPOLLIN);
	struct tenant *tenant = evt->data.ptr;
	assert(tenant);

	/* Accept Client Request as a nonblocking socket, saving address information */
	struct sockaddr_in client_address;
	socklen_t          address_length = sizeof(client_address);

	/* Accept as many clients requests as possible, returning when we would have blocked */
	while (true) {
		int client_socket = accept4(tenant->tcp_server.socket_descriptor, (struct sockaddr *)&client_address,
		                            &address_length, SOCK_NONBLOCK);
		if (unlikely(client_socket < 0)) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) return;

			panic("accept4: %s", strerror(errno));
		}

		on_client_request_arrival(client_socket, (const struct sockaddr *)&client_address, tenant);
	}
}

static void
on_metrics_server_epoll_event(struct epoll_event *evt)
{
	assert((evt->events & EPOLLIN) == EPOLLIN);

	/* Accept Client Request as a nonblocking socket, saving address information */
	struct sockaddr_in client_address;
	socklen_t          address_length = sizeof(client_address);

	/* Accept as many clients requests as possible, returning when we would have blocked */
	while (true) {
		/* We accept the client connection with blocking semantics because we spawn ephemeral worker threads */
		int client_socket = accept4(metrics_server.tcp.socket_descriptor, (struct sockaddr *)&client_address,
		                            &address_length, 0);
		if (unlikely(client_socket < 0)) {
			if (errno == EWOULDBLOCK || errno == EAGAIN) return;

			panic("accept4: %s", strerror(errno));
		}

		metrics_server_thread_spawn(client_socket);
	}
}

static void
on_client_socket_epoll_event(struct epoll_event *evt)
{
	assert(evt);

	struct http_session *session = evt->data.ptr;
	assert(session);

	listener_thread_unregister_http_session(session);

	switch (session->state) {
	case HTTP_SESSION_RECEIVE_REQUEST_BLOCKED:
		assert((evt->events & EPOLLIN) == EPOLLIN);
		on_client_request_receiving(session);
		break;
	case HTTP_SESSION_SEND_RESPONSE_HEADER_BLOCKED:
		assert((evt->events & EPOLLOUT) == EPOLLOUT);
		on_client_response_header_sending(session);
		break;
	case HTTP_SESSION_SEND_RESPONSE_BODY_BLOCKED:
		assert((evt->events & EPOLLOUT) == EPOLLOUT);
		on_client_response_body_sending(session);
		break;
	default:
		panic("Invalid HTTP Session State");
	}
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
thread_local static struct priority_queue *global_request_scheduler_minheap;
noreturn void *
listener_thread_main(void *dummy)
{
	thread_id = 200;
	struct epoll_event epoll_events[RUNTIME_MAX_EPOLL_EVENTS];

	metrics_server_init();
	listener_thread_register_metrics_server();

	for (int i = 0; i < 1024; i++) {
		generator[i] = NULL;
		rate[i] = -1;
		change[i] = false;
		input[i] = -1;
		g_session[i] = NULL;
	}
	
	while (true) {
		/* Block indefinitely on the epoll file descriptor, waiting on up to a max number of events */
		int descriptor_count = epoll_wait(listener_thread_epoll_file_descriptor, epoll_events,
		                                  RUNTIME_MAX_EPOLL_EVENTS, -1);
		if (descriptor_count < 0) {
			if (errno == EINTR) continue;

			panic("epoll_wait: %s", strerror(errno));
		}

		/* Assumption: Because epoll_wait is set to not timeout, we should always have descriptors here */
		assert(descriptor_count > 0);

		for (int i = 0; i < descriptor_count; i++) {
			panic_on_epoll_error(&epoll_events[i]);

			enum epoll_tag tag = *(enum epoll_tag *)epoll_events[i].data.ptr;

			switch (tag) {
			case EPOLL_TAG_TENANT_SERVER_SOCKET:
				on_tenant_socket_epoll_event(&epoll_events[i]);
				break;
			case EPOLL_TAG_HTTP_SESSION_CLIENT_SOCKET:
				on_client_socket_epoll_event(&epoll_events[i]);
				break;
			case EPOLL_TAG_METRICS_SERVER_SOCKET:
				on_metrics_server_epoll_event(&epoll_events[i]);
				break;
			default:
				panic("Unknown epoll type!");
			}
		}
	}

	panic("Listener thread unexpectedly broke loop\n");
}
