#!/bin/bash
coros=(1 2 3 4)
threads=(36 32 24 18 16 12 8 4 2 1)
machines=(3)

for machine in ${machines[@]}; do
	for thread in ${threads[@]}; do
		for coro in ${coros[@]}  ; do
			# if [[ ($coro -gt 1) ]] ; then
			# 	continue 
			# fi
			echo "./run_ser.sh"
			./run_ser.sh
			sleep 15
			echo "./run_cli.sh $thread $coro $machine > thropt_${thread}_${coro}_${machine}.thropt"
			timeout 1800s ./run_cli.sh ${thread} ${coro} ${machine} > thropt_${thread}_${coro}_${machine}.thropt
			echo "./latancy.sh > lat_${thread}_${coro}_${machine}.latency"
			./latency.sh > lat_${thread}_${coro}_${machine}.latency
			echo "./kill_ser.sh"
			./kill_ser.sh
			sleep 8
		done done done
