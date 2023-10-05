#pragma once

#include <stdbool.h>

#include "sandbox_types.h"

/* Returns pointer back if successful, null otherwise */
typedef void (*local_runqueue_add_fn_t)(struct sandbox *);
typedef void (*local_runqueue_add_fn_t_idx)(int index, struct sandbox *);
typedef uint64_t (*local_runqueue_try_add_fn_t_idx)(int index, struct sandbox *, bool *need_interrupt);
typedef bool (*local_runqueue_is_empty_fn_t)(void);
typedef void (*local_runqueue_delete_fn_t)(struct sandbox *sandbox);
typedef struct sandbox *(*local_runqueue_get_next_fn_t)();
typedef int (*local_runqueue_get_height_fn_t)();

struct local_runqueue_config {
	local_runqueue_add_fn_t          add_fn;
	local_runqueue_add_fn_t_idx      add_fn_idx;
	local_runqueue_try_add_fn_t_idx  try_add_fn_idx;
	local_runqueue_is_empty_fn_t     is_empty_fn;
	local_runqueue_delete_fn_t       delete_fn;
	local_runqueue_get_next_fn_t     get_next_fn;
	local_runqueue_get_height_fn_t   get_height_fn;
};

void            local_runqueue_add(struct sandbox *);
void            local_runqueue_add_index(int index, struct sandbox *);
uint64_t        local_runqueue_try_add_index(int index, struct sandbox *, bool *need_interrupt);
void            local_runqueue_delete(struct sandbox *);
bool            local_runqueue_is_empty();
struct sandbox *local_runqueue_get_next();
void            local_runqueue_initialize(struct local_runqueue_config *config);
int             local_runqueue_get_height();
