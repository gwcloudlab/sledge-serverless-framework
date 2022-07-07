RUNTIME_DIR=../../../runtime/
SLEDGE_BINARY_DIR=${RUNTIME_DIR}/bin
SLEDGE_TESTS_DIR=${RUNTIME_DIR}/tests
HOSTNAME=10.10.1.1
DURATION_SEC=1

all: run

clean: 
	make -C ${RUNTIME_DIR} clean
	make -C ${SLEDGE_TESTS_DIR} clean
	rm -f ${SLEDGE_BINARY_DIR}/fibonacci.wasm.so

${SLEDGE_BINARY_DIR}/sledgert:
	make -C ${RUNTIME_DIR} runtime

.PHONY: sledgert
sledgert: ${SLEDGE_BINARY_DIR}/sledgert

${SLEDGE_BINARY_DIR}/fibonacci.wasm.so:
	make -C ../../../applications fibonacci.install

.PHONY: fibonacci
fibonacci: ${SLEDGE_BINARY_DIR}/fibonacci.wasm.so

run: sledgert fibonacci
	LD_LIBRARY_PATH=${SLEDGE_BINARY_DIR} ${SLEDGE_BINARY_DIR}/sledgert spec.json

debug: sledgert fibonacci
	SLEDGE_DISABLE_PREEMPTION=true SLEDGE_NWORKERS=1 \
	LD_LIBRARY_PATH=${SLEDGE_BINARY_DIR} gdb ${SLEDGE_BINARY_DIR}/sledgert \
		--eval-command="handle SIGUSR1 noprint nostop" \
		--eval-command="handle SIGPIPE noprint nostop" \
		--eval-command="set pagination off" \
		--eval-command="run spec.json"

client-fib30-once:
	echo "30" | http ${HOSTNAME}:10000

client-fib30-multi:
	hey -z ${DURATION_SEC}s -cpus 2 -c 100 -t 0 -o csv -m GET -d "30\n" "http://${HOSTNAME}:10000"

client-fib30_005p-once:
	echo "30" | http ${HOSTNAME}:10005

client-fib30_005p-multi:
	hey -z ${DURATION_SEC}s -c 100 -t 0 -o csv -m GET -d "30\n" "http://${HOSTNAME}:10005"

client-fib30_020p-once:
	echo "30" | http ${HOSTNAME}:10020

client-fib30_020p-multi:
	hey -z ${DURATION_SEC}s -c 100 -t 0 -o csv -m GET -d "30\n" "http://${HOSTNAME}:10020"

client-fib30_050p-once:
	echo "30" | http ${HOSTNAME}:10050

client-fib30_050p-multi:
	hey -z ${DURATION_SEC}s -c 72 -t 0 -o csv -m GET -d "30\n" "http://${HOSTNAME}:10050"

client-fib30_100p-once:
	echo "30" | http ${HOSTNAME}:10100

client-fib30_100p-multi:
	hey -z ${DURATION_SEC}s -c 100 -t 0 -o csv -m GET -d "30\n" "http://${HOSTNAME}:10100"


client-preempt:
	(echo "40" | http ${HOSTNAME}:10040 &); echo "10" | http ${HOSTNAME}:10010

client-fib40-once:
	echo "40" | http ${HOSTNAME}:10040

client-fib40-multi:
	hey -z ${DURATION_SEC}s -cpus 4 -c 100 -t 0 -o csv -m GET -d "40\n" "http://${HOSTNAME}:10040"

client-admin:
	echo "11" | http ${HOSTNAME}:11111

client-terminator:
	echo "55" | http ${HOSTNAME}:55555
