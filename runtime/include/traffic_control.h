#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "module.h"

#define ADMISSIONS_CONTROL_GRANULARITY_DBF 1000 // 1ms
#define TRAFFIC_CONTROL
// #define LOG_TRAFFIC_CONTROL

void traffic_control_initialize(void);
void traffic_control_log_decision(const int admissions_case_num, const bool admitted);
int  traffic_control_decide(struct module *module, uint64_t absolute_deadline, uint64_t estimated_execution);
