#!/bin/bash


#cores_list=(1 2 4 10 20 40 50 60 77)
#cores_list=(50 60)
#cores_list=(1 2 4 6 8 10 20 30 40 50 60 70 77)

#cores_list=(1 2 4 6 8 10 12 14 16 18 20 24 28)
cores_list=(1)
#rate=(10000 25000 35000 50000 100000 200000)
rate=(35000)
#cores_list=(20)
#cores_list=(1 2 4 6 8 10 12 14)
ulimit -n 1000000
#./kill_sledge.sh
for(( r=0;r<${#rate[@]}; r++))
do
	echo "experiment rate ${rate[r]}"
	for(( i=0;i<${#cores_list[@]};i++ )) do
		server_log="server-"${cores_list[i]}".log"
       		sudo ./no_hyperthread.sh > yves
       		echo "sledge start with worker core ${cores_list[i]}"
		./start.sh ${cores_list[i]}
#		./start.sh ${cores_list[i]} > $server_log 2>&1 &
		sleep 8 
		curl -H 'Expect:' -H "Content-Type: application/json" --data-binary "0 ${rate[r]} 25" "http://localhost:10030/fib"
		echo fib
		curl -H 'Expect:' -H "Content-Type: application/json" --data-binary "1 ${rate[r]} 15" "http://localhost:10031/empty"
		echo empty
		sleep 50
		./kill_sledge.sh
		grep throughput $server_log | wc -l 
	done
#	mkdir "multi_sandbox/fifo/"${rate[r]}
	mv server-* "multi_sandbox/fifo/"${rate[r]}
done	

