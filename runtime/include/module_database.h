#pragma once

#include "module.h"

#define MODULE_DATABASE_CAPACITY 128

extern struct module *module_database[MODULE_DATABASE_CAPACITY];
extern size_t         module_database_count;

int            module_database_add(struct module *module);
struct module *module_database_find_by_name(char *name);
struct module *module_database_find_by_socket_descriptor(int socket_descriptor);
struct module *module_database_find_by_port(uint16_t port);
