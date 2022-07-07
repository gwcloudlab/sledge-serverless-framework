#include "estimated_exec_info.h"
#include "debuglog.h"
#include "panic.h"
#include "perf_window.h"

/**
 * Initializes perf window
 * @param estimated_exec_info
 */
void
estimated_exec_info_initialize(struct estimated_exec_info *estimated_exec_info, uint8_t percentile,
                               uint64_t expected_execution)
{
#ifdef TRAFFIC_CONTROL
	assert(expected_execution > 0);
	estimated_exec_info->estimated_execution = expected_execution;

	perf_window_initialize(&estimated_exec_info->perf_window);

	if (unlikely(percentile < 50 || percentile > 99)) panic("Invalid percentile");
	estimated_exec_info->percentile = percentile;

	estimated_exec_info->control_index = PERF_WINDOW_BUFFER_SIZE * percentile / 100;
#ifdef LOG_TRAFFIC_CONTROL
	debuglog("Percentile: %u\n", estimated_exec_info->percentile);
	debuglog("Control Index: %u\n", estimated_exec_info->control_index);
#endif
#endif
}


/*
 * Adds an execution value to the perf window and calculates and caches and updated estimate
 * @param estimated_exec_info
 * @param execution_duration
 */
void
estimated_exec_info_update(struct estimated_exec_info *estimated_exec_info, uint64_t execution_duration)
{
#ifdef TRAFFIC_CONTROL
	struct perf_window *perf_window = &estimated_exec_info->perf_window;

	LOCK_LOCK(&estimated_exec_info->perf_window.lock);
	perf_window_add(perf_window, execution_duration);
	estimated_exec_info->estimated_execution = perf_window_get_percentile(perf_window,
	                                                                      estimated_exec_info->percentile,
	                                                                      estimated_exec_info->control_index);
	LOCK_UNLOCK(&estimated_exec_info->perf_window.lock);
#endif
}
