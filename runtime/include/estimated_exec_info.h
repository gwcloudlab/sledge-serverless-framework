#pragma once

#include "perf_window_t.h"

#define TRAFFIC_CONTROL
// #define LOG_TRAFFIC_CONTROL

struct estimated_exec_info {
	struct perf_window perf_window;
	uint8_t            percentile;          /* 50 - 99 */
	uint8_t            control_index;       /* Precomputed Lookup index when perf_window is full */
	uint64_t           estimated_execution; /* cycles */
};

void estimated_exec_info_initialize(struct estimated_exec_info *admissions_info, uint8_t percentile,
                                    uint64_t expected_execution);
void estimated_exec_info_update(struct estimated_exec_info *admissions_info, uint64_t execution_duration);
