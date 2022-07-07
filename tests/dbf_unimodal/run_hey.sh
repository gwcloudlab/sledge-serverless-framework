#!/bin/bash

#shellcheck disable=SC1091

# This experiment is intended to document how the level of concurrent requests influence the latency, throughput, and success rate
# Success - The percentage of requests that complete by their deadlines
# Throughput - The mean number of successful requests per second
# Latency - the rount-trip resonse time (us) of successful requests at the p50, p90, p99, and p100 percentiles

# Add bash_libraries directory to path
__run_sh__base_path="$(dirname "$(realpath --logical "${BASH_SOURCE[0]}")")"
__run_sh__bash_libraries_relative_path="../bash_libraries"
__run_sh__bash_libraries_absolute_path=$(cd "$__run_sh__base_path" && cd "$__run_sh__bash_libraries_relative_path" && pwd)
export PATH="$__run_sh__bash_libraries_absolute_path:$PATH"

source csv_to_dat.sh || exit 1
source framework.sh || exit 1
source generate_gnuplots.sh || exit 1
source get_result_count.sh || exit 1
source panic.sh || exit 1
source path_join.sh || exit 1
source percentiles_table.sh || exit 1

validate_dependencies hey gnuplot jq

# The global configs for the scripts
declare -r SERVER_LOG_FILE="perf.log"
declare -r CLIENT_TERMINATE_SERVER=true
declare -r ITERATIONS=10000 # ignored when DURATION_sec is used
declare -r DURATION_sec=30
declare -r NWORKERS=$(($(nproc)-2))
declare -r INIT_PORT=10000
declare -r APP=fib30
declare -r ARG=30
declare -r EXPECTED_EXEC_us=4000
# declare -r DEADLINE_CLIENT_US=21000
# declare -r RESPONSE_DELAY=1000
# declare -r DEADLINE_SERVER_US=$((DEADLINE_CLIENT_US-RESPONSE_DELAY))
declare -r DEADLINE_US=16000
declare -r MTDS_REPL_PERIOD_us=0
declare -r MTDS_MAX_BUDGET_us=0

# There must be only TWO values below, the first one must be zero!
declare -ar MTDBF_RESERVATIONS=(0 100) # for quick testing

generate_spec() {
	printf "Generating 'spec.json'...(may take a couple sec)\n"

	local -i port

	for ru in "${MTDBF_RESERVATIONS[@]}"; do
		workload=$(printf "%s_%03dp" "$APP" "$ru")
		port=$((INIT_PORT+ru))

		# Generates unique module specs on different ports using the given 'ru's
		jq ". + { \
		\"name\": \"${workload}\",\
		\"port\": ${port},\
		\"expected-execution-us\": ${EXPECTED_EXEC_us},\
		\"relative-deadline-us\": ${DEADLINE_US},\
		\"replenishment-period-us\": ${MTDS_REPL_PERIOD_us}, \
		\"max-budget-us\": ${MTDS_MAX_BUDGET_us}, \
		\"reservation-percentile\": $ru}" \
			< "./template.json" \
			> "./result_${ru}.json"
	done

	jq ". + { \
	\"name\": \"Admin\",\
	\"port\": 11111,\
	\"expected-execution-us\": ${EXPECTED_EXEC_us},\
	\"relative-deadline-us\": ${DEADLINE_US},\
	\"replenishment-period-us\": 0, \
	\"max-budget-us\": 0, \
	\"reservation-percentile\": 0}" \
		< "./template.json" \
		> "./result_admin.json"

	jq ". + { \
	\"name\": \"Terminator\",\
	\"port\": 55555,\
	\"expected-execution-us\": ${EXPECTED_EXEC_us},\
	\"relative-deadline-us\": ${DEADLINE_US},\
	\"replenishment-period-us\": 0, \
	\"max-budget-us\": 0, \
	\"reservation-percentile\": 0}" \
		< "./template.json" \
		> "./result_terminator.json"

	# Merges all of the multiple specs for a single module
	jq -s '. | sort_by(.name)' ./result_*.json > "./spec.json"
	rm ./result_*.json
}


# Execute the experiments concurrently
# $1 (hostname)
# $2 (results_directory) - a directory where we will store our results
run_experiments() {
	if (($# != 2)); then
		panic "invalid number of arguments \"$1\""
		return 1
	elif [[ -z "$1" ]]; then
		panic "hostname \"$1\" was empty"
		return 1
	elif [[ ! -d "$2" ]]; then
		panic "directory \"$2\" does not exist"
		return 1
	fi

	local hostname="$1"
	local results_directory="$2"

	# The duration in seconds that the low priority task should run before the high priority task starts
	local -ir OFFSET=1

	printf "Running Experiments\n"

	# Run concurrently
	# The lower priority has OFFSETs to ensure it runs the entire time the high priority is trying to run
	# This asynchronously trigger jobs and then wait on their pids
	local app_g_PID
	local app_ng_PID

	local -r workload_ng=$(printf "%s_%03dp" "$APP" 0)
	local -r port_ng=$INIT_PORT

	local -r con_ng=$((NWORKERS*10)) #144
	local -r con_g=$((NWORKERS*4)) #18*4 FULL LOAD

	for ru in "${MTDBF_RESERVATIONS[@]}"; do
		if [ "$ru" == 0 ]; then
			continue
		fi
		workload_g=$(printf "%s_%03dp" "$APP" "$ru")
		port_g=$((INIT_PORT+ru))

		# hey -disable-compression -disable-keepalive -disable-redirects -z $((DURATION_sec+OFFSET+1))s -n $ITERATIONS -c $con_ng -t 0 -o csv -m GET -d $ARG "http://${hostname}:${port_ng}" > "$results_directory/$workload_ng.csv" 2> "$results_directory/$workload_ng-err.dat" &
		# app_ng_PID="$!"

		# sleep "$OFFSET"s
		
		hey -disable-compression -disable-keepalive -disable-redirects -z ${DURATION_sec}s -n "$ITERATIONS" -c $con_g -t 0 -o csv -m GET -d $ARG "http://${hostname}:${port_g}" > "$results_directory/$workload_g.csv" 2> "$results_directory/$workload_g-err.dat" &
		app_g_PID="$!"

		wait -f "$app_g_PID" || {
			printf "\t%s: [ERR]\n" "$workload_g"
			panic "failed to wait -f $app_g_PID"
			return 1
		}
		get_result_count "$results_directory/$workload_g.csv" || {
			printf "\t%s: [ERR]\n" "$workload_g"
			panic "$workload_g has zero requests."
			return 1
		}
		printf "\t%s: [OK]\n" "$workload_g"

		# wait -f "$app_ng_PID" || {
		# 	printf "\t%s: [ERR]\n" "$workload_ng"
		# 	panic "failed to wait -f $app_ng_PID"
		# 	return 1
		# }
		# get_result_count "$results_directory/$workload_ng.csv" || {
		# 	printf "\t%s: [ERR]\n" "$workload_ng"
		# 	panic "$workload_ng has zero requests."
		# 	return 1
		# }
		# printf "\t%s: [OK]\n" "$workload_ng"
	done

	if [ "$CLIENT_TERMINATE_SERVER" == true ]; then
		printf "Sent a Terminator to the server\n"
		echo "55" | http "$hostname":55555 &> /dev/null
	fi

	return 0
}

# Process the experimental results and generate human-friendly results for success rate, throughput, and latency
process_client_results() {
	if (($# != 1)); then
		error_msg "invalid number of arguments ($#, expected 1)"
		return 1
	elif ! [[ -d "$1" ]]; then
		error_msg "directory $1 does not exist"
		return 1
	fi

	local -r results_directory="$1"

	printf "Processing Results: "

	# Write headers to CSVs
	printf "Res%%,Scs%%,TOTAL,ClientScs,All200,AllFail,Deny,MisDL,Shed,MiscErr\n" >> "$results_directory/success.csv"
	printf "Res%%,Throughput\n" >> "$results_directory/throughput.csv"
	percentiles_table_header "$results_directory/latency.csv" "Res%%"
	# percentiles_table_header "$results_directory/latency-200.csv" "Res%%"

	for ru in "${MTDBF_RESERVATIONS[@]}"; do
		# if [ "$ru" == 0 ]; then
		# 	continue
		# fi
		workload=$(printf "%s_%03dp" "$APP" "$ru")

		# Some requests come back with an "Unsolicited response ..." See issue #185
		misc_err=$(wc -l < "$results_directory/$workload-err.dat")

		# Calculate Success Rate for csv (percent of requests that return 200 within DEADLINE_US)
		awk -v misc_err="$misc_err" -F, '
			$7 == 200 && ($1 * 1000000) <= '"$DEADLINE_US"' {ok++}
			$7 == 200 {all200++}
			$7 != 200 {total_failed++}
			$7 == 429 {denied++}
			$7 == 408 {missed_dl++}
			$7 == 409 {killed++}
			END{printf "'"$ru"',%3.1f,%d,%d,%d,%d,%d,%d,%d,%d\n", (ok / (NR-1+misc_err) * 100), (NR-1+misc_err), ok, all200, (total_failed-1+misc_err), denied, missed_dl, killed, misc_err}
		' < "$results_directory/$workload.csv" >> "$results_directory/success.csv"

		# Convert from s to us, and sort
		awk -F, 'NR > 1 {print ($1 * 1000000)}' < "$results_directory/$workload.csv" \
			| sort -g > "$results_directory/$workload-response.csv"
		
		# Filter on 200s, convert from s to us, and sort
		awk -F, '$7 == 200 {print ($1 * 1000000)}' < "$results_directory/$workload.csv" \
			| sort -g > "$results_directory/$workload-response-200.csv"

		# Get Number of 200s
		all=$(wc -l < "$results_directory/$workload-response.csv")
		((all == 0)) && continue # If all errors, skip line
		
		# Get Number of 200s
		oks=$(wc -l < "$results_directory/$workload-response-200.csv")
		#((oks == 0)) && continue # If all errors, skip line

		# We determine duration by looking at the timestamp of the last complete request
		# TODO: Should this instead just use the client-side synthetic DURATION_sec value?
		duration=$(tail -n1 "$results_directory/$workload.csv" | cut -d, -f8)

		# Throughput is calculated as the mean number of SUCCESSFULL requests per second
		throughput=$(echo "$oks/$duration" | bc)
		printf "%s,%d\n" "$ru" "$throughput" >> "$results_directory/throughput.csv"

		# Generate Latency Data for csv
		percentiles_table_row "$results_directory/$workload-response.csv" "$results_directory/latency.csv" "$ru"
		# percentiles_table_row "$results_directory/$workload-response-200.csv" "$results_directory/latency-200.csv" "$ru"

		# Delete scratch file used for sorting/counting
		rm -rf "$results_directory/$workload-response.csv" 
		rm -rf "$results_directory/$workload-response-200.csv"
	done

	# Transform csvs to dat files for gnuplot
	csv_to_dat "$results_directory/success.csv" "$results_directory/throughput.csv" "$results_directory/latency.csv"
	rm "$results_directory/success.csv" "$results_directory/throughput.csv" "$results_directory/latency.csv"

	# Generate gnuplots
	generate_gnuplots "$results_directory" "$__run_sh__base_path" || {
		printf "[ERR]\n"
		panic "failed to generate gnuplots"
	}

	printf "[OK]\n"
	return 0
}

process_server_results() {
	local -r results_directory="${1:?results_directory not set}"

	if ! [[ -d "$results_directory" ]]; then
		error_msg "directory $1 does not exist"
		return 1
	fi

	printf "Processing Server Results: "

	num_of_lines=$(wc -l < "$results_directory/$SERVER_LOG_FILE")
	if [ "$num_of_lines" == 1 ]; then
		printf "\nNo results to process! Exiting the script."
		return 1
	fi

	# Write headers to CSVs
	printf "Res%%,Scs%%,TOTAL,SrvScs,All200,AllFail,DenyAny,DenyG,MisDL_Glb,MisDL_Loc,Shed_Glb,Shed_Loc,QueFull\n" >> "$results_directory/success.csv"
	printf "Res%%,Throughput\n" >> "$results_directory/throughput.csv"
	percentiles_table_header "$results_directory/latency.csv" "Res%%"

	# local -a metrics=(total queued uninitialized allocated initialized runnable interrupted preempted running_sys running_user asleep returned complete error)
	local -a metrics=(total queued running_user asleep)

	local -A fields=(
		[total]=6
		[queued]=7
		[uninitialized]=8
		[allocated]=9
		[initialized]=10
		[runnable]=11
		[interrupted]=12
		[preempted]=13
		[running_sys]=14
		[running_user]=15
		[asleep]=16
		[returned]=17
		[complete]=18
		[error]=19
	)

	local -r proc_MHz_idx=20
	local -r response_code_idx=21

	# Write headers to CSVs
	for metric in "${metrics[@]}"; do
		percentiles_table_header "$results_directory/$metric.csv" "module"
	done
	percentiles_table_header "$results_directory/running_user_200.csv" "module"
	percentiles_table_header "$results_directory/running_user_nonzero.csv" "module"
	percentiles_table_header "$results_directory/total_200.csv" "module"
	# percentiles_table_header "$results_directory/memalloc.csv" "module"

	for ru in "${MTDBF_RESERVATIONS[@]}"; do
		if [ "$ru" == 0 ]; then
			continue
		fi
		workload=$(printf "%s_%03dp" "$APP" "$ru")
		mkdir "$results_directory/$workload"

		for metric in "${metrics[@]}"; do
			awk -F, '$2 == "'"$workload"'" {printf("%d,%d\n", $'"${fields[$metric]}"' / $'"$proc_MHz_idx"', $'"$response_code_idx"')}' < "$results_directory/$SERVER_LOG_FILE" | sort -g > "$results_directory/$workload/${metric}_sorted.csv"

			percentiles_table_row "$results_directory/$workload/${metric}_sorted.csv" "$results_directory/${metric}.csv" "$workload"

			# Delete scratch file used for sorting/counting
			# rm "$results_directory/$workload/${metric}_sorted.csv"
		done

		awk -F, '$2 == 200 {printf("%d,%d\n", $1, $2)}' < "$results_directory/$workload/running_user_sorted.csv" > "$results_directory/$workload/running_user_200_sorted.csv"
		percentiles_table_row "$results_directory/$workload/running_user_200_sorted.csv" "$results_directory/running_user_200.csv" "$workload"
		awk -F, '$1 > 0 {printf("%d,%d\n", $1, $2)}' < "$results_directory/$workload/running_user_sorted.csv" > "$results_directory/$workload/running_user_nonzero_sorted.csv"
		percentiles_table_row "$results_directory/$workload/running_user_nonzero_sorted.csv" "$results_directory/running_user_nonzero.csv" "$workload"
		awk -F, '$2 == 200 {printf("%d,%d\n", $1, $2)}' < "$results_directory/$workload/total_sorted.csv" > "$results_directory/$workload/total_200_sorted.csv"
		percentiles_table_row "$results_directory/$workload/total_200_sorted.csv" "$results_directory/total_200.csv" "$workload"


		# Memory Allocation
		# awk -F, '$2 == "'"$workload"'" {printf("%.0f\n", $20)}' < "$results_directory/$SERVER_LOG_FILE" | sort -g > "$results_directory/$workload/memalloc_sorted.csv"
		# percentiles_table_row "$results_directory/$workload/memalloc_sorted.csv" "$results_directory/memalloc.csv" "$workload" "%1.0f"


		# Calculate Success Rate for csv (percent of requests that complete), $1 and DEADLINE_US are both in us, so not converting 
		# awk -F, '
		# 	$1 <= '"$DEADLINE_US"' {ok++}
		# 	END{printf "'"$ru"',%3.1f\n", (ok / NR * 100)}
		# ' < "$results_directory/$workload/total_sorted.csv" >> "$results_directory/success.csv"

		awk -F, '
			$2 == 200 && $1 <= '"$DEADLINE_US"' {ok++}
			$2 == 200 {all200++}
			$2 != 200 {total_failed++}
			$2 == 4290 {denied_any++}
			$2 == 4291 {denied_gtd++}
			$2 == 4080 {mis_dl_glob++}
			$2 == 4081 {mis_dl_local++}
			$2 == 4090 {shed_glob++}
			$2 == 4091 {shed_local++}
			$2 == 999 {global_full++}
			END{printf "'"$ru"',%3.1f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", (ok / NR * 100), NR, ok, all200, total_failed, denied_any, denied_gtd, mis_dl_glob, mis_dl_local, shed_glob, shed_local, global_full}
		' < "$results_directory/$workload/total_sorted.csv" >> "$results_directory/success.csv"

		# Throughput is calculated on the client side, so ignore the below line
		printf "%s,%d\n" "$ru" "1" >> "$results_directory/throughput.csv"

		# Generate Latency Data for csv
		percentiles_table_row "$results_directory/$workload/total_sorted.csv" "$results_directory/latency.csv" "$ru"
		

		# Delete scratch file used for sorting/counting
		# rm "$results_directory/$workload/memalloc_sorted.csv"

		# Delete directory
		# rm -rf "${results_directory:?}/${workload:?}"

	done

	# Transform csvs to dat files for gnuplot
	for metric in "${metrics[@]}"; do
		csv_to_dat "$results_directory/$metric.csv"
		rm "$results_directory/$metric.csv"
	done
	csv_to_dat "$results_directory/running_user_200.csv" "$results_directory/running_user_nonzero.csv" "$results_directory/total_200.csv"
	rm "$results_directory/running_user_200.csv" "$results_directory/running_user_nonzero.csv" "$results_directory/total_200.csv"

	# csv_to_dat "$results_directory/memalloc.csv"
	csv_to_dat "$results_directory/success.csv" "$results_directory/throughput.csv" "$results_directory/latency.csv"

	# rm "$results_directory/memalloc.csv" 
	rm "$results_directory/success.csv" "$results_directory/throughput.csv" "$results_directory/latency.csv"

	# Generate gnuplots
	generate_gnuplots "$results_directory" "$__run_sh__base_path" || {
		printf "[ERR]\n"
		panic "failed to generate gnuplots"
	}

	printf "[OK]\n"
	return 0
}

experiment_server_post() {
	local -r results_directory="$1"

	# Only process data if SLEDGE_SANDBOX_PERF_LOG was set when running sledgert
	if [[ -n "$SLEDGE_SANDBOX_PERF_LOG" ]]; then
		if [[ -f "$__run_sh__base_path/$SLEDGE_SANDBOX_PERF_LOG" ]]; then
			mv "$__run_sh__base_path/$SLEDGE_SANDBOX_PERF_LOG" "$results_directory/$SERVER_LOG_FILE"
			process_server_results "$results_directory" || return 1
			rm "$results_directory/$SLEDGE_SANDBOX_PERF_LOG"
		else
			echo "Perf Log was set, but $SERVER_LOG_FILE not found!"
		fi
	fi
}

# Expected Symbol used by the framework
experiment_client() {
	local -r target_hostname="$1"
	local -r results_directory="$2"

	#run_samples "$target_hostname" || return 1
	run_experiments "$target_hostname" "$results_directory" || return 1
	process_client_results "$results_directory" || return 1

	return 0
}

generate_spec
framework_init "$@"
