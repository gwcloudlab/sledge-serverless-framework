#pragma once

#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>

#include "admissions_control.h"
#include "admissions_info.h"
#include "http.h"
#include "panic.h"
#include "software_interrupt.h"
#include "types.h"

/* Wasm initialization functions generated by the compiler */
#define MODULE_INITIALIZE_GLOBALS "populate_globals"
#define MODULE_INITIALIZE_MEMORY  "populate_memory"
#define MODULE_INITIALIZE_TABLE   "populate_table"
#define MODULE_INITIALIZE_LIBC    "wasmf___init_libc"
#define MODULE_MAIN               "wasmf_main"

#define MODULE_DEFAULT_REQUEST_RESPONSE_SIZE (PAGE_SIZE)

#define MODULE_MAX_ARGUMENT_COUNT 16
#define MODULE_MAX_ARGUMENT_SIZE  64
#define MODULE_MAX_MODULE_COUNT   128
#define MODULE_MAX_NAME_LENGTH    32
#define MODULE_MAX_PATH_LENGTH    256

/*
 * Defines the listen backlog, the queue length for completely established socketeds waiting to be accepted
 * If this value is greater than the value in /proc/sys/net/core/somaxconn (typically 128), then it is silently
 * truncated to this value. See man listen(2) for info
 *
 * When configuring the number of sockets to handle, the queue length of incomplete sockets defined in
 * /proc/sys/net/ipv4/tcp_max_syn_backlog should also be considered. Optionally, enabling syncookies removes this
 * maximum logical length. See tcp(7) for more info.
 */
#define MODULE_MAX_PENDING_CLIENT_REQUESTS 128
#if MODULE_MAX_PENDING_CLIENT_REQUESTS > 128
#warning \
  "MODULE_MAX_PENDING_CLIENT_REQUESTS likely exceeds the value in /proc/sys/net/core/somaxconn and thus may be silently truncated";
#endif

struct module {
	char                        name[MODULE_MAX_NAME_LENGTH];
	char                        path[MODULE_MAX_PATH_LENGTH];
	void *                      dynamic_library_handle; /* Handle to the *.so of the serverless function */
	int32_t                     argument_count;
	uint32_t                    stack_size; /* a specification? */
	uint64_t                    max_memory; /* perhaps a specification of the module. (max 4GB) */
	uint32_t                    relative_deadline_us;
	uint64_t                    relative_deadline; /* cycles */
	_Atomic uint32_t            reference_count;   /* ref count how many instances exist here. */
	struct indirect_table_entry indirect_table[INDIRECT_TABLE_SIZE];
	struct sockaddr_in          socket_address;
	int                         socket_descriptor;
	struct admissions_info      admissions_info;
	int                         port;

	/*
	 * unfortunately, using UV for accepting connections is not great!
	 * on_connection, to create a new accepted connection, will have to init a tcp handle,
	 * which requires a uvloop. cannot use main as rest of the connection is handled in
	 * sandboxing threads, with per-core(per-thread) tls data-structures.
	 * so, using direct epoll for accepting connections.
	 */

	unsigned long max_request_size;
	char          request_headers[HTTP_MAX_HEADER_COUNT][HTTP_MAX_HEADER_LENGTH];
	int           request_header_count;
	char          request_content_type[HTTP_MAX_HEADER_VALUE_LENGTH];

	/* resp size including headers! */
	unsigned long max_response_size;
	int           response_header_count;
	char          response_content_type[HTTP_MAX_HEADER_VALUE_LENGTH];
	char          response_headers[HTTP_MAX_HEADER_COUNT][HTTP_MAX_HEADER_LENGTH];

	/* Equals the largest of either max_request_size or max_response_size */
	unsigned long max_request_or_response_size;

	/* Functions to initialize aspects of sandbox */
	mod_glb_fn_t  initialize_globals;
	mod_mem_fn_t  initialize_memory;
	mod_tbl_fn_t  initialize_tables;
	mod_libc_fn_t initialize_libc;

	/* Entry Function to invoke serverless function */
	mod_main_fn_t main;
};

/*************************
 * Public Static Inlines *
 ************************/

/**
 * Increment a modules reference count
 * @param module
 */
static inline void
module_acquire(struct module *module)
{
	assert(module->reference_count < UINT32_MAX);
	atomic_fetch_add(&module->reference_count, 1);
	return;
}

/**
 * Get a module's argument count
 * @param module
 * @returns the number of arguments
 */
static inline int32_t
module_get_argument_count(struct module *module)
{
	return module->argument_count;
}

/**
 * Invoke a module's initialize_globals
 * @param module
 */
static inline void
module_initialize_globals(struct module *module)
{
	/* called in a sandbox. */
	module->initialize_globals();
}

/**
 * Invoke a module's initialize_tables
 * @param module
 */
static inline void
module_initialize_table(struct module *module)
{
	/* called at module creation time (once only per module). */
	module->initialize_tables();
}

/**
 * Invoke a module's initialize_libc
 * @param module - module whose libc we are initializing
 * @param env - address?
 * @param arguments - address?
 */
static inline void
module_initialize_libc(struct module *module, int32_t env, int32_t arguments)
{
	/* called in a sandbox. */
	module->initialize_libc(env, arguments);
}

/**
 * Invoke a module's initialize_memory
 * @param module - the module whose memory we are initializing
 */
static inline void
module_initialize_memory(struct module *module)
{
	// called in a sandbox.
	module->initialize_memory();
}

/**
 * Validate module, defined as having a non-NULL dynamical library handle and entry function pointer
 * @param module - module to validate
 */
static inline void
module_validate(struct module *module)
{
	/* Assumption: Software Interrupts are disabled by caller */
	assert(!software_interrupt_is_enabled());

	if (!module) {
		panic("module %p | module is unexpectedly NULL\n", module);
	} else if (!module->dynamic_library_handle) {
		panic("module %p | module->dynamic_library_handle is unexpectedly NULL\n", module);
	} else if (!module->main) {
		panic("module %p | module->main is unexpectedly NULL\n", module);
	}
}

/**
 * Invoke a module's entry function, forwarding on argc and argv
 * @param module
 * @param argc standard UNIX count of arguments
 * @param argv standard UNIX vector of arguments
 * @return return code of module's main function
 */
static inline int32_t
module_main(struct module *module, int32_t argc, int32_t argv)
{
	return module->main(argc, argv);
}

/**
 * Decrement a modules reference count
 * @param module
 */
static inline void
module_release(struct module *module)
{
	assert(module->reference_count > 0);
	atomic_fetch_sub(&module->reference_count, 1);
	return;
}

/********************************
 * Public Methods from module.c *
 *******************************/

void           module_free(struct module *module);
struct module *module_new(char *mod_name, char *mod_path, int32_t argument_count, uint32_t stack_sz, uint32_t max_heap,
                          uint32_t relative_deadline_us, int port, int req_sz, int resp_sz, int admissions_percentile,
                          uint32_t expected_execution_us);
int            module_new_from_json(char *filename);
