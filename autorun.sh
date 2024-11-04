#!/bin/bash
coros=(1 2 3 4)
threads=(1 2 4 8 12 16 18 24 32 38)
machines=(3)

for machine in ${machines[@]}; do
	for thread in ${threads[@]}; do
		for coro in ${coros[@]}  ; do
			echo "./run_ser.sh"
			./run_ser.sh
			sleep 8
			echo "./run_cli.sh $thread $coro $machine > thropt_${thread}_${coro}_${machine}.thropt"
			./run_cli.sh ${thread} ${coro} ${machine} # > thropt_${thread}_${coro}_${machine}.thropt
			echo "./latancy.sh > lat_${thread}_${coro}_${machine}.latency"
			./latancy.sh > lat_${thread}_${coro}_${machine}.latency
			echo "./kill_ser.sh"
			./kill_ser.sh
			sleep 10
		done done done
