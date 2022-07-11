#ifndef DBF_H
#define DBF_H

#include "panic.h"
#include "module.h"
#include "runtime.h"
#include "arch/getcycles.h"

struct module;

typedef enum
{
	DBF_CHECK_AND_ADD_DEMAND,               /* normal mode for adding new sandbox demands */
	DBF_FORCE_ADD_NEW_SANDBOX_DEMAND,       /* work-conservation mode*/
	DBF_REDUCE_EXISTING_DEMAND,             /* normal mode for reducing existing sandbox demands */
	DBF_CHECK_EXISTING_SANDBOX_EXTRA_DEMAND /* special case when a sandbox goes over its expected exec */
	// DBF_DORCE_ADD_EXTRA_DEMAND  /* case when a sandbox goes over its expected exec */
} dbf_update_mode_t;

struct dbf {
	int      worker_idx;
	uint32_t capacity;
	uint64_t max_relative_deadline;
	uint64_t base_supply; /* supply amount for time 1 */
	uint64_t demands[];
};

struct message {
	struct module             *module;
	struct sandbox            *sandbox;
	struct sandbox_metadata *sandbox_meta;
	uint64_t                   sandbox_id;
	uint16_t                   sandbox_response_code; ///// TODO temp
	uint64_t                   absolute_deadline;
	uint64_t                   adjustment;
	uint8_t                    state;
	int64_t                    remaining_execution;
	int                        sender_worker_idx;
	dbf_update_mode_t          dbf_update_mode;
	// bool                       extra_demand_request_approved;
	bool                       exceeded_estimation;
	uint64_t                   last_extra_demand_timestamp;

	int      if_case;
	int64_t  prev_rem_exec;
	uint64_t last_exec_dur;
	uint8_t  reserv;
};

extern uint64_t    time_granularity;
extern struct dbf *global_dbf;

static inline void
dbf_print(struct dbf *dbf)
{
	assert(dbf != NULL);

	printf("DBF INFO:\n\
	\t WorkerIDX: \t%d\n\
	\t Capacity: \t%u\n\
	\t Max DL: \t%lu\n\
	\t Basic Supply: \t%lu\n\n",
	       dbf->worker_idx, dbf->capacity, dbf->max_relative_deadline, dbf->base_supply);

	for (int i = 0; i < dbf->capacity; i++) {
		if (dbf->demands[i] > 0) printf("demands[%d] = %lu\n", i, dbf->demands[i]);
	}
}

static inline struct dbf *
dbf_initialize(uint32_t num_of_workers, uint8_t reservation_percentile, int worker_idx)
{
	struct dbf *dbf = (struct dbf *)calloc(1, sizeof(struct dbf) + sizeof(uint64_t) * 1);

	dbf->capacity              = 1;
	dbf->max_relative_deadline = 0;
	dbf->worker_idx            = worker_idx;
	uint32_t cpu_factor        = (num_of_workers == 1) ? 1 : num_of_workers * RUNTIME_MAX_CPU_UTIL_PERCENTILE / 100;
	dbf->base_supply           = time_granularity * cpu_factor * reservation_percentile / 100;

	return dbf;
}

static inline struct dbf *
dbf_grow(struct dbf *dbf, uint64_t new_max_relative_deadline)
{
	assert(dbf != NULL);

	uint32_t new_capacity = new_max_relative_deadline / time_granularity; // NOT adding 1 for final leftovers
	// printf("new_cap = %u\n", new_capacity);

	struct dbf *new_dbf = realloc(dbf, sizeof(struct dbf) + sizeof(uint64_t) * new_capacity);
	if (new_dbf == NULL) panic("Failed to grow dbf\n");

	// TODO: need to memset to 0 explicitly? YES! MUST DO. Tested!
	memset(new_dbf->demands, 0, new_capacity * sizeof(uint64_t));

	new_dbf->capacity              = new_capacity;
	new_dbf->max_relative_deadline = new_max_relative_deadline;

	return new_dbf;
}

static inline bool
dbf_check_supply_quick(struct dbf *dbf, uint64_t start_time, uint64_t sandbox_absolute_deadline, uint64_t adjustment)
{
	assert(dbf != NULL);
	assert(start_time < sandbox_absolute_deadline);

	const uint32_t live_deadline_len        = (sandbox_absolute_deadline - start_time) / time_granularity;
	const uint32_t absolute_destination_idx = (sandbox_absolute_deadline / time_granularity - 1) % dbf->capacity;
	const uint64_t max_supply_at_deadline     = live_deadline_len*dbf->base_supply;

	return (dbf->demands[absolute_destination_idx] + adjustment <= max_supply_at_deadline);
}

static inline bool
dbf_try_update_demand(struct dbf *dbf, uint64_t start_time, uint64_t module_relative_deadline,
                      uint64_t sandbox_absolute_deadline, uint64_t adjustment, dbf_update_mode_t dbf_update_mode,
                      struct message *new_message)
{
	assert(dbf != NULL);
	assert(start_time < sandbox_absolute_deadline);

	bool demand_is_below_supply = true;

	const uint32_t module_relative_deadline_len = module_relative_deadline / time_granularity;
	const uint32_t live_deadline_len            = (sandbox_absolute_deadline - start_time)
	                                   / time_granularity; // + 1; // add 1 until blocking is resolved?
	const uint32_t absolute_destination_idx = (sandbox_absolute_deadline / time_granularity - 1) % dbf->capacity;

	for (uint32_t i = absolute_destination_idx, iter = 0;
	     i < absolute_destination_idx + module_relative_deadline_len; i++, iter++) {
		uint32_t circular_i = i % dbf->capacity;

		const uint64_t max_supply_at_time_i = (live_deadline_len + iter) * dbf->base_supply;
		const uint64_t prev_demand          = dbf->demands[circular_i];

		switch (dbf_update_mode) {
		case DBF_CHECK_AND_ADD_DEMAND:
			dbf->demands[circular_i] += adjustment;

			if (dbf->demands[circular_i] > max_supply_at_time_i) {
				/* Undo DBF adding if over supply detected */
				for (uint32_t j = absolute_destination_idx; j <= i; j++) {
					dbf->demands[j % dbf->capacity] -= adjustment;
				}
				goto err_demand_over_supply;
			}
			break;
		case DBF_CHECK_EXISTING_SANDBOX_EXTRA_DEMAND:
			if (dbf->demands[circular_i] + adjustment > max_supply_at_time_i) goto err_demand_over_supply;
			break;
		// case DBF_CHECK_EXISTING_SANDBOX_EXTRA_DEMAND:
		case DBF_FORCE_ADD_NEW_SANDBOX_DEMAND:
			/* [Work Conservation Scenario] Only applicable for module dbf! */
			dbf->demands[circular_i] += adjustment;
			assert(/* adjustment > 0 && */ prev_demand < dbf->demands[circular_i]);

			if (dbf->demands[circular_i] > max_supply_at_time_i) { demand_is_below_supply = false; }
			break;
		case DBF_REDUCE_EXISTING_DEMAND:
			dbf->demands[circular_i] -= adjustment;
			if (/* adjustment < 0 && */ prev_demand < dbf->demands[circular_i]) {
				printf("DBF_REDUCE_EXISTING_DEMAND\n");
				printf("Worker ID: %d\n", dbf->worker_idx);
				printf("Module Reservation: %u\n", new_message->reserv);
				printf("Sandbox ID: %lu\n", new_message->sandbox_id);
				printf("Sandbox Response Code: %u\n", new_message->sandbox_response_code);
				printf("Basic supply: %lu\n", dbf->base_supply);
				printf("Cap=%u\n", dbf->capacity);
				printf("Abs_dest_idx=%u\n", absolute_destination_idx);
				printf("live_deadline_len=%u\n", live_deadline_len);
				printf("i=%u, cir_i = %u, iter = %u\n", i, circular_i, iter);
				printf("max_supply_at_time_i = %lu\n\n", max_supply_at_time_i);
				printf("Prev_demand[%u]=%lu\n\n", circular_i, prev_demand);
				printf("demand[%u]=%lu\n\n", circular_i, dbf->demands[circular_i]);
				printf("sandbox_state=%u, if_case=%d\n", new_message->state, new_message->if_case);
				printf("exceeded_estimation=%d\n", new_message->exceeded_estimation);
				printf("Adjustment=%lu\n", adjustment);
				printf("last_exec_duration=%lu, prev_rem_exec=%ld, rem_exec=%ld\n",
				       new_message->last_exec_dur, new_message->prev_rem_exec,
				       new_message->remaining_execution);

				dbf_print(dbf);
				panic("Interger Underflow -> Tried reducing demand, but it actually went over supply!");
			}
			break;
		}
	}

done:
	return demand_is_below_supply;
err_demand_over_supply:
	demand_is_below_supply = false;
	goto done;
}

static inline uint64_t
dbf_get_demand_overgone_its_supply_at(struct dbf *dbf, uint64_t start_time, uint64_t sandbox_absolute_deadline)
{
	assert(dbf != NULL);
	assert(start_time < sandbox_absolute_deadline);

	const uint32_t live_deadline_len    = (sandbox_absolute_deadline - start_time) / time_granularity;
	const uint32_t absolute_arrival_idx = start_time / time_granularity % dbf->capacity;

	uint64_t demand_overgone = 0;

	for (uint32_t i = absolute_arrival_idx, iter = 0; i < absolute_arrival_idx + live_deadline_len; i++, iter++) {
		uint32_t circular_i = i % dbf->capacity;

		const uint64_t max_supply_at_time_i  = (live_deadline_len + iter) * dbf->base_supply;
		const uint64_t curr_demand_at_time_i = dbf->demands[circular_i];

		if (curr_demand_at_time_i > max_supply_at_time_i) {
			if (curr_demand_at_time_i - max_supply_at_time_i > demand_overgone) {
				demand_overgone = curr_demand_at_time_i - max_supply_at_time_i;
			}
			// if (dbf->base_supply > 0) {
			// 	printf("DBF_AVAILABLE_SUPPLY_AT\n");
			// 	printf("Worker ID: %d\n", dbf->worker_idx);
			// 	printf("Basic supply: %lu\n", dbf->base_supply);
			// 	printf("Cap=%u\n", dbf->capacity);
			// 	printf("Abs_arrival_idx=%u\n", absolute_arrival_idx);
			// 	printf("live_deadline_len=%u\n", live_deadline_len);
			// 	printf("i=%u, cir_i = %u, iter = %u\n", i, circular_i, iter);
			// 	printf("curr_demand_at_time_i = %lu\n", curr_demand_at_time_i);
			// 	printf("max_supply_at_time_i = %lu\n\n", max_supply_at_time_i);
			// 	dbf_print(dbf);
			// }
		}
	}

	return demand_overgone;
}

static inline uint64_t
dbf_get_available_supply_at(struct dbf *dbf, uint64_t module_relative_deadline, uint64_t sandbox_absolute_deadline,
                            uint8_t reservation_p)
{
	assert(dbf != NULL);

	const uint32_t relative_deadline_len    = module_relative_deadline / time_granularity;
	const uint32_t absolute_destination_idx = (sandbox_absolute_deadline / time_granularity - relative_deadline_len)
	                                            % dbf->capacity
	                                          + relative_deadline_len - 1;

	uint64_t remaining_supply = UINT64_MAX;

	for (uint32_t i = absolute_destination_idx, iter = 0; i < absolute_destination_idx + relative_deadline_len;
	     i++, iter++) {
		uint32_t circular_i = i % dbf->capacity;

		const uint64_t max_supply_at_time_i  = (relative_deadline_len + iter) * dbf->base_supply;
		const uint64_t curr_demand_at_time_i = dbf->demands[circular_i];

		assert(curr_demand_at_time_i <= max_supply_at_time_i);

		if (curr_demand_at_time_i == max_supply_at_time_i) {
			// printf("DBF_AVAILABLE_SUPPLY_AT\n");
			// printf("Worker ID: %d\n", dbf->worker_idx);
			// printf("Module Reservation: %u\n",reservation_p);
			// printf("Basic supply: %lu\n", dbf->base_supply);
			// printf("Cap=%u\n", dbf->capacity);
			// printf("Abs_dest_idx=%u\n", absolute_destination_idx);
			// printf("Relative_len=%u\n", relative_deadline_len);
			// printf("i=%u, cir_i = %u, iter = %u\n", i, circular_i, iter);
			// printf("curr_demand_at_time_i = %lu\n", curr_demand_at_time_i);
			// printf("max_supply_at_time_i = %lu\n\n", max_supply_at_time_i);
			// printf("demand[%u]=%lu\n\n", circular_i, dbf->demands[circular_i]);
			// panic("Worker %d has gone over its supply!", dbf->worker_idx);
			goto no_supply_left;
		}

		if (max_supply_at_time_i - curr_demand_at_time_i < remaining_supply) {
			remaining_supply = max_supply_at_time_i - curr_demand_at_time_i;
		}
	}

done:
	return remaining_supply;

no_supply_left:
	remaining_supply = 0;
	goto done;
}

static inline void
dbf_free(struct dbf *dbf)
{
	assert(dbf != NULL);

	free(dbf);
}


#endif /* DBF_H */
