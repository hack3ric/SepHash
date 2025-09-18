#!/bin/bash

scale=10000000
malloc_size=508468256  # 484M, 10^7
cache_ratios=(0.01 0.05 0.1 0.2 0.4 0.6 0.8 1.0)
read_ratios=(100 95 50)
insert_ratios=(0 5 50)
my_ip="10.30.2.9"

for _uniform in {0..1}; do
    for _cache_ratio in ${cache_ratios[@]}; do
        for _i in {0..2}; do
            _cache_size=$(echo "$malloc_size * $_cache_ratio / 1" | bc)
            _read_ratio=${read_ratios[$_i]}
            _insert_ratio=${insert_ratios[$_i]}
            if [ $_uniform -eq 1 ]; then
                _pattern_type=1
            else
                _pattern_type=2
            fi
            echo running uniform=$_uniform cache_ratio=$_cache_ratio cache_size=$_cache_size read=$_read_ratio insert=$_insert_ratio

            build/ser_cli --server --gid_idx 1 --max_coro 256 --cq_size 64 --mem_size 91268055040 &
            _memory_pid=$!

            sleep 30

            # TODO: warmup
            PTH_BM_FILENAME="sephash.csv" \
                PTH_BM_EXTRA_COLS="cache_ratio" \
                PTH_BM_EXTRA_COL_VALUES="$_cache_ratio" \
                SEPHASH_CBUF_SIZE=$_cache_size \
                build/ser_cli \
                    --server_ip 127.0.0.1 --num_machine 1 --num_cli 1 --num_coro 1 \
                    --gid_idx 1 \
                    --max_coro 256 --cq_size 64 \
                    --machine_id 0 \
                    --load_num $scale \
                    --num_op $scale \
                    --pattern_type $_pattern_type \
                    --read_frac "$(echo "scale=2; x=$_read_ratio/100; if (x<1) print 0; x" | bc)" \
                    --insert_frac "$(echo "scale=2; x=$_insert_ratio/100; if (x<1) print 0; x" | bc)" \
                    --update_frac  0.0 \
                    --delete_frac  0.0 \
                    --read_size     64
            kill -9 $_memory_pid
            sleep 10
        done
    done
done
