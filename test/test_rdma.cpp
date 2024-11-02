/*
 * The MIT License (MIT)
 *
 * Copyright (C) 2022-2023 Feng Ren, Tsinghua University 
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <iostream>
#include <cassert>
#include <unistd.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <immintrin.h>
#include <atomic>
#include <random>
#include <numa.h>
#include <common.h>
#include <config.h>

#include <aiordma.h>
#include <sephash.h>
// #include "smart/thread.h"
// #include "smart/initiator.h"
// #include "smart/target.h"


static const size_t MEM_POOL_SIZE = (1ull << 30);

void run_server(uint16_t port) {
    rdma_dev dev( "mlx5_0" , 1 , 1 ) ; // dev_name, _ib_port, _gid_idx 
    rdma_server server( dev ) ;
    ibv_mr *seg_mr = dev.reg_mr( 233 , MEM_POOL_SIZE ) ;
    server.start_serve() ;
    log_info("server starts, press c to exit");
    while (getchar() != 'c');
    server.stop_serve() ;
}

int run_flag = 0 ;
int MAX_CLI  = 500 ;
int num_cli  = 1 ;
int num_coro = 1 ;
size_t block_size = 8; 
int qp_num = -1 ;
std::string type = "read" ;
std::string dump_file_path;
std::string dump_prefix = "rdma-" + type ;

std::vector<rdma_client*> rdma_clis  ( MAX_CLI + 1 , nullptr ) ;
std::vector<rdma_conn*>   rdma_conns ( MAX_CLI + 1 , nullptr ) ;
std::vector<ibv_mr*   >   lmrs       ( MAX_CLI + 1 , nullptr ) ;

std::atomic<uint64_t> total_attempts(0);
volatile int stop_signal = 0;
pthread_barrier_t barrier;

static inline void BindCore(int core_id) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core_id, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

task<> test_thread_func( int thread_id , int coro_id ) { 
    rdma_client* cli = rdma_clis [thread_id] ;
    rdma_conn*  conn = rdma_conns[thread_id] ;
    ibv_mr*      lmr = lmrs      [thread_id*num_coro + coro_id] ;
    rdma_rmr seg_rmr = cli->run( conn->query_remote_mr( 233 ) ) ;

    // BindCore( thread_id );
    size_t kSegmentSize = MEM_POOL_SIZE / num_cli ;
    
    while( run_flag == 0 ) ; // barrier

    uint64_t attempts = 0;
    std::mt19937 rnd;
    std::uniform_int_distribution<uint32_t> dist(0, kSegmentSize / block_size - 1);
    if (type == "read") {
        while (!stop_signal) {
            attempts++;
            uint32_t offset = thread_id * kSegmentSize + block_size * (dist(rnd));
            co_await conn->read( seg_rmr.raddr + offset , seg_rmr.rkey , lmr->addr , block_size , lmr->lkey ) ; 

        }
    } else if (type == "write") {
        while (!stop_signal) {
            attempts++;
            uint32_t offset = thread_id * kSegmentSize + block_size * (dist(rnd));
            co_await conn->write( seg_rmr.raddr + offset , seg_rmr.rkey , lmr->addr , block_size , lmr->lkey ) ;
 
        }
    } else if (type == "atomic") {
        assert(block_size == 8);
        while (!stop_signal) {
            attempts++;
            uint32_t offset = thread_id * kSegmentSize + block_size * (dist(rnd));
            co_await conn->faa( seg_rmr.raddr + offset , seg_rmr.rkey , 8 ) ; 
        }
    }
    total_attempts.fetch_add(attempts); 
    co_return;
}

double connect_time = 0.0;

void report(uint64_t elapsed_time) {
    auto bandwidth = total_attempts * block_size / elapsed_time / 1024.0 / 1024.0;
    auto throughput = total_attempts / elapsed_time / 1000000.0;
    printf("%s: #threads=%d, #coro=%d, #block_size=%ld, BW=%.3lf MB/s, IOPS=%.3lf M/s, conn establish time=%.3lf ms\n",
             dump_prefix.c_str(), num_cli , num_coro, block_size, bandwidth, throughput, connect_time);
    if (dump_file_path.empty()) {
        return;
    }
    FILE *fout = fopen(dump_file_path.c_str(), "a+");
    if (!fout) {
        log_err("fopen");
        return;
    }
    fprintf(fout, "%s, %d, %ld, %.3lf, %.3lf, %.3lf\n",
            dump_prefix.c_str(), num_cli ,  block_size, bandwidth, throughput, connect_time);
    fclose(fout);
}

void run_client(const std::vector<std::string> &server_list, uint16_t port) {
    struct timeval start_tv, end_tv;
     
    rdma_dev dev( "mlx5_0" , 1 , 1 ) ;
    uint64_t cbuf_size = ( 1ul<<20 ) ;
    char*    mem_buf   = (char*) malloc( cbuf_size * ( num_cli * num_coro + 1 ) ) ; 

    gettimeofday(&start_tv, NULL);
    for( int i = 0 ; i < num_cli ; i ++ ){
        rdma_clis[i]  = new rdma_client( dev , so_qp_cap , rdma_default_tempmp_size , 256 , 64 ) ;
        rdma_conns[i] = rdma_clis[i]->connect( server_list[0].c_str() ) ; // only one server
        for( int j = 0 ; j < num_coro ; j ++ ){
            lmrs[i*num_coro+j] = dev.create_mr( cbuf_size , mem_buf + cbuf_size * ( i * num_coro + j ) ) ;
        }  
        assert( rdma_conns[i] != nullptr ) ;
    }
    gettimeofday(&end_tv, NULL);
    connect_time = (end_tv.tv_sec - start_tv.tv_sec) * 1000.0 +
                   (end_tv.tv_usec - start_tv.tv_usec) / 1000.0;
    
     
    std::thread ths[80];
    for (uint64_t i = 0; i < num_cli; i++) {
        auto th = [&]( rdma_client *rdma_cli , int cli_id ) {
            std::vector<task<>> tasks;
            for (uint64_t j = 0; j < num_coro; j++)
            {
                tasks.emplace_back( test_thread_func( cli_id , j ) );
            }
            rdma_cli->run(gather(std::move(tasks)));
        };
        ths[i] = std::thread( th , rdma_clis[i] , i );
    }
    gettimeofday(&start_tv, NULL);
    run_flag = 1 ;
    sleep(15);
    stop_signal = 1;
    for (uint64_t i = 0; i < num_cli; i++) {
        ths[i].join();
    }

    gettimeofday(&end_tv, NULL); 
    double elapsed_time = (end_tv.tv_sec - start_tv.tv_sec) * 1.0 +
                   (end_tv.tv_usec - start_tv.tv_usec) / 1000000.0;
    report(elapsed_time);
}

int main(int argc, char **argv) {
    int port = 10001 ;
    // BindCore(0);

    if (argc == 1) {
        run_server(port);

    } else {
        num_cli = argc < 2 ? 1 : atoi(argv[1]) ; 
        num_coro = argc < 3 ? 1 : atoi(argv[2]) ; 
        block_size = argc < 4 ? block_size : atoi( argv[3] ) ;
        std::vector<std::string> server_list = {"10.77.110.158"};
        assert(!server_list.empty());
        run_client(server_list, port);
    }
    return 0;
}
