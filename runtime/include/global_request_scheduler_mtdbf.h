#pragma once

#include "global_request_scheduler.h"

void global_request_scheduler_mtdbf_initialize();
int  global_request_scheduler_mtdbf_shed_work(struct module *, uint64_t, uint64_t);
