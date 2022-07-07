#include <unistd.h>

#include "traffic_control.h"
#include "debuglog.h"
#include "global_request_scheduler_mtdbf.h"


struct dbf *global_dbf;
uint64_t    time_granularity;

// static struct module *bad_module = NULL; //////// temp

void
traffic_control_initialize()
{
#ifdef TRAFFIC_CONTROL
	time_granularity = (uint64_t)runtime_processor_speed_MHz * ADMISSIONS_CONTROL_GRANULARITY_DBF; // 1ms
	printf("Admissions Time Granularity is %lu\n", time_granularity);

	global_dbf = dbf_initialize(runtime_worker_threads_count, 100, -1);
#endif
}

void
traffic_control_log_decision(const int admissions_case_num, const bool admitted)
{
#ifdef LOG_TRAFFIC_CONTROL
	debuglog("Admission case number: %d, Admitted? %s\n", admissions_case_num, admitted ? "yes" : "no");
#endif /* LOG_TRAFFIC_CONTROL */
}

int
traffic_control_decide(struct module *module, const uint64_t absolute_deadline, const uint64_t estimated_execution)
{
	/* Nominal non-zero value in case traffic control is disabled */
	uint64_t work_admitted = 1;

#ifdef TRAFFIC_CONTROL
	// if (bad_module == NULL && module->reservation_percentile == 0) {
	// 	bad_module = module;
	// 	debuglog("Setting Bad Module!");
	// }

	struct message temp_message = { .module = module, .if_case = 70, .reserv = module->reservation_percentile };

	bool global_can_admit = dbf_try_update_demand(global_dbf, absolute_deadline - module->relative_deadline,
	                                              module->relative_deadline, absolute_deadline, estimated_execution,
	                                              DBF_CHECK_AND_ADD_DEMAND, &temp_message);
	temp_message.if_case  = 71;
	bool module_can_admit = dbf_try_update_demand(module->module_dbf, absolute_deadline - module->relative_deadline,
	                                              module->relative_deadline, absolute_deadline, estimated_execution,
	                                              DBF_CHECK_AND_ADD_DEMAND, &temp_message);

	if (module_can_admit && global_can_admit) {
		/* Case #1: Both the tenant and overall system is under utlized. So, just admit. */
		traffic_control_log_decision(1, true);
	} else if (!module_can_admit && global_can_admit) {
		/* Case #2: Tenant is over utilized, but system is under utilized. So, admit for work-conservation. */
		temp_message.if_case = 722;
		dbf_try_update_demand(module->module_dbf, absolute_deadline - module->relative_deadline,
		                      module->relative_deadline, absolute_deadline, estimated_execution,
		                      DBF_FORCE_ADD_NEW_SANDBOX_DEMAND, &temp_message);
		traffic_control_log_decision(2, true);
	} else if (module_can_admit && !global_can_admit) {
		/* Case #3: Tenant is under utilized, but  system is over utilized. So, shed work and then admit. */
		while (!global_can_admit) {
			int rc = global_request_scheduler_mtdbf_shed_work(NULL,
			                                                  absolute_deadline - module->relative_deadline,
			                                                  absolute_deadline);
			if (rc != 0) {
				/* This case is not desirable, where there is no bad module requests left in the global
				 * queue, so we have deny the guaranteed tenant job. */
				temp_message.if_case = 733;
				dbf_try_update_demand(module->module_dbf, absolute_deadline - module->relative_deadline,
				                      module->relative_deadline, absolute_deadline, estimated_execution,
				                      DBF_REDUCE_EXISTING_DEMAND, &temp_message);

				traffic_control_log_decision(3, false);
				goto guaranteed_work_not_admitted;
			}

			temp_message.if_case = 734;
			global_can_admit     = dbf_try_update_demand(global_dbf,
			                                             absolute_deadline - module->relative_deadline,
			                                             module->relative_deadline, absolute_deadline,
			                                             estimated_execution, DBF_CHECK_AND_ADD_DEMAND,
			                                             &temp_message);
		}

		traffic_control_log_decision(3, true);
	} else if (!module_can_admit && !global_can_admit) {
		/* Case #4: Do NOT admit. */
		traffic_control_log_decision(4, false);
		goto any_work_not_admitted;
	}
#endif /* TRAFFIC_CONTROL */

done:
	return work_admitted;
any_work_not_admitted:
	work_admitted = 4290;
	goto done;
guaranteed_work_not_admitted:
	work_admitted = 4291;
	goto done;
}
