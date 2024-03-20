#!/bin/bash
ulimit -n 655350

function usage {
        echo "$0 [worker num] [listener num] [first worker core id]"
        exit 1
}

if [ $# != 3 ] ; then
        usage
        exit 1;
fi

worker_num=$1
listener_num=$2
first_worker_core_id=$3

declare project_path="$(
        cd "$(dirname "$0")/../.."
        pwd
)"
echo $project_path
path=`pwd`
export SLEDGE_DISABLE_PREEMPTION=true
export SLEDGE_DISABLE_BUSY_LOOP=true
export SLEDGE_DISABLE_AUTOSCALING=true
export SLEDGE_DISABLE_EXPONENTIAL_SERVICE_TIME_SIMULATION=true
#export SLEDGE_SIGALRM_HANDLER=TRIAGED
export SLEDGE_FIRST_WORKER_COREID=$first_worker_core_id
export SLEDGE_NWORKERS=$worker_num
export SLEDGE_NLISTENERS=$listener_num
export SLEDGE_WORKER_GROUP_SIZE=9
export SLEDGE_SCHEDULER=EDF
#export SLEDGE_DISPATCHER=DARC
#export SLEDGE_DISPATCHER=SHINJUKU
export SLEDGE_DISPATCHER=EDF_INTERRUPT
export SLEDGE_SANDBOX_PERF_LOG=$path/server.log
#echo $SLEDGE_SANDBOX_PERF_LOG
cd $project_path/runtime/bin
#LD_LIBRARY_PATH="$(pwd):$LD_LIBRARY_PATH" ./sledgert ../tests/test_fibonacci.json
#LD_LIBRARY_PATH="$(pwd):$LD_LIBRARY_PATH" ./sledgert ../tests/test_big_fibonacci.json
#LD_LIBRARY_PATH="$(pwd):$LD_LIBRARY_PATH" ./sledgert ../tests/test_armcifar10.json
#LD_LIBRARY_PATH="$(pwd):$LD_LIBRARY_PATH" ./sledgert ../tests/test_png2bmp.json
#LD_LIBRARY_PATH="$(pwd):$LD_LIBRARY_PATH" ./sledgert ../tests/test_image_processing.json
#LD_LIBRARY_PATH="$(pwd):$LD_LIBRARY_PATH" ./sledgert ../tests/mulitple_linear_chain.json
#LD_LIBRARY_PATH="$(pwd):$LD_LIBRARY_PATH" ./sledgert ../tests/test_multiple_image_processing.json
#LD_LIBRARY_PATH="$(pwd):$LD_LIBRARY_PATH" ./sledgert ../tests/test_multiple_image_processing3.json
#LD_LIBRARY_PATH="$(pwd):$LD_LIBRARY_PATH" ./sledgert ../tests/fib.json
LD_LIBRARY_PATH="$(pwd):$LD_LIBRARY_PATH" ./sledgert ../tests/hash.json
#LD_LIBRARY_PATH="$(pwd):$LD_LIBRARY_PATH" ./sledgert ../tests/empty.json
#LD_LIBRARY_PATH="$(pwd):$LD_LIBRARY_PATH" ./sledgert ../tests/my_fibonacci.json
#LD_LIBRARY_PATH="$(pwd):$LD_LIBRARY_PATH" ./sledgert ../tests/test_sodresize.json
#LD_LIBRARY_PATH="$(pwd):$LD_LIBRARY_PATH" ./sledgert ../tests/my_sodresize.json

