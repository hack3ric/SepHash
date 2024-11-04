#include "clevel.h"
#include "clevel_filter.h"
#include "clevel_single_filter.h"
#include "generator.h"
#include "plush.h"
#include "plush_single_filter.h"
#include "race.h"
#include "sephash.h"
#include "sephash_zip.h"
#include "split_batch.h"
#include "split_hash.h"
#include "split_hash_idle.h"
#include "split_inline_dep.h"
#include "split_search_base.h"
#include "split_search_fptable.h"
#include "split_search_fptable_wocache.h"
#include <set>
#include <stdint.h>
#include <atomic>

// #define ORDERED_INSERT
Config config;
uint64_t load_num;
using ClientType = RACE::ClientMultiShard;
using ServerType = RACE::Server;
using Slice = RACE::Slice;

std::atomic<long long> op_counter ;
bool start_flag ;

inline uint64_t GenKey(uint64_t key)
{
#ifdef ORDERED_INSERT
    return key;
#else
    return FNVHash64(key);
#endif
}

template <class Client>
    requires KVTrait<Client, Slice *, Slice *>
task<> load(Client *cli, uint64_t cli_id, uint64_t coro_id)
{
    co_await cli->start(config.num_machine * config.num_cli * config.num_coro);
    uint64_t tmp_key;
    Slice key, value;
    std::string tmp_value = std::string(8, '1');
    value.len = tmp_value.length();
    value.data = (char *)tmp_value.data();
    key.len = sizeof(uint64_t);
    key.data = (char *)&tmp_key;
    uint64_t load_avr = load_num / (config.num_machine * config.num_cli * config.num_coro);
    uint64_t shardid = config.machine_id * config.num_cli * config.num_coro + cli_id * config.num_coro + coro_id ;
    while( !start_flag ) ;
    for (uint64_t i = 1 ; ; i++)
    {
        if( ( i - 1 ) % 100 == 0 ){
            long long tmp = op_counter.fetch_add( 100 ) ;
            if( tmp >= config.load_num / config.num_machine ) break ;
        }
        tmp_key = GenKey( shardid * load_avr + i);
        co_await cli->insert(&key, &value);
    }
    co_await cli->stop();
    co_return;
}

template <class Client>
    requires KVTrait<Client, Slice *, Slice *>
task<> run(Generator *gen, Client *cli, uint64_t cli_id, uint64_t coro_id)
{
    co_await cli->start(config.num_machine * config.num_cli * config.num_coro);
    uint64_t tmp_key;
    char buffer[1024];
    Slice key, value, ret_value, update_value;

    ret_value.data = buffer;

    std::string tmp_value = std::string(8, '1');
    value.len = tmp_value.length();
    value.data = (char *)tmp_value.data();

    std::string tmp_value_2 = std::string(8, '2');
    update_value.len = tmp_value_2.length();
    update_value.data = (char *)tmp_value_2.data();

    key.len = sizeof(uint64_t);
    key.data = (char *)&tmp_key;

    double op_frac;
    double read_frac = config.insert_frac + config.read_frac;
    double update_frac = config.insert_frac + config.read_frac + config.update_frac;
    xoshiro256pp op_chooser;
    xoshiro256pp key_chooser;
    uint64_t load_avr = load_num / (config.num_machine * config.num_cli * config.num_coro);
    uint64_t op_avr   = config.num_op / ( config.num_machine * config.num_cli * config.num_coro ) ;
    uint64_t shardid = config.machine_id * config.num_cli * config.num_coro + cli_id * config.num_coro + coro_id ;
    // uint64_t num_op = config.num_op / (config.num_machine * config.num_cli * config.num_coro);
    while( !start_flag ) ;
    for (uint64_t i = 1 ; ; i++)
    {
        if( ( i - 1 ) % 100 == 0 ){
            long long tmp = op_counter.fetch_add( 100 ) ;
            if( tmp >= config.num_op / config.num_machine ) {
                // log_err( "finish: %lu node, %lu thr, %lu coro runs %lu operations" , config.machine_id, cli_id , coro_id , i - 1 ) ;
                break ;
            }
            // if( ( i - 1 ) % 1000000 == 0 )
            //     log_err( "%lu node, %lu thr, %lu coro runs %lu operations" , config.machine_id, cli_id , coro_id , i - 1 ) ;  
        }
        op_frac = op_chooser();
        if (op_frac < config.insert_frac)
        {
            tmp_key = GenKey(
                load_num + shardid * op_avr + gen->operator()(key_chooser()) );
            co_await cli->insert(&key, &value);
        }
        else if (op_frac < read_frac)
        {
            ret_value.len = 0;
            tmp_key = GenKey( shardid * load_avr + gen->operator()(key_chooser()));
            co_await cli->search(&key, &ret_value);
        }
        else if (op_frac < update_frac)
        {
            // update
            tmp_key = GenKey( shardid * load_avr + gen->operator()(key_chooser()));
            co_await cli->update(&key, &update_value);
        }
        else
        {
            // delete
            tmp_key = GenKey( shardid * load_avr + gen->operator()(key_chooser()));
            co_await cli->remove(&key);
        }
    }
    co_await cli->stop();
    co_return;
}

int main(int argc, char *argv[])
{
    config.ParseArg(argc, argv);
    load_num = config.load_num;
    if (config.is_server)
    {
        ServerType ser(config);
        while (true)
            ;
    }
    else
    {
        uint64_t cbuf_size = (1ul << 20) * 20;
        char *mem_buf = (char *)malloc(cbuf_size * (config.server_num * config.num_cli * config.num_coro + 1));
        // rdma_dev dev("mlx5_1", 1, config.gid_idx);
        rdma_dev dev("mlx5_0", 1, config.gid_idx);
        // rdma_dev dev(nullptr, 1, config.gid_idx);
        std::vector<ibv_mr *> lmrs(config.num_cli * config.num_coro + 1, nullptr);
        std::vector<rdma_client *> rdma_clis(config.num_cli + 1, nullptr);
        std::vector<rdma_conn *> rdma_conns(config.num_cli + 1, nullptr);
        std::vector<rdma_conn *> rdma_wowait_conns(config.num_cli + 1, nullptr);
        std::mutex dir_lock;
        std::vector<BasicDB *> clis;
        std::thread ths[80];

        for (uint64_t i = 0; i < config.server_num; i++) {
            log_err("%s", config.server_ip[i]);
        }

        for (uint64_t i = 0; i < config.num_cli; i++)
        {
            rdma_clis[i] = new rdma_client(dev, so_qp_cap, rdma_default_tempmp_size, config.max_coro, config.cq_size);
            for (uint64_t j = 0; j < config.num_coro; j++)
            {
                BasicDB *cli;
                cli = new ClientType(config, dev, mem_buf, cbuf_size,
                                    rdma_clis[i], config.machine_id, i, j);
                clis.push_back(cli);
            }
        }

        // For Rehash Thread
        std::atomic_bool exit_flag{true};
        bool rehash_flag = typeid(ClientType) == typeid(CLEVEL::Client) ||
                           typeid(ClientType) == typeid(ClevelFilter::Client) ||
                           typeid(ClientType) == typeid(ClevelSingleFilter::Client);
        ;

        printf("Load start\n");
        start_flag = false ;
        op_counter.store( 0 ) ;
        for (uint64_t i = 0; i < config.num_cli; i++)
        {
            auto th = [&](rdma_client *rdma_cli, uint64_t cli_id) {
                std::vector<task<>> tasks;
                for (uint64_t j = 0; j < config.num_coro; j++)
                {
                    tasks.emplace_back(load((ClientType *)clis[cli_id * config.num_coro + j], cli_id, j));
                }
                rdma_cli->run(gather(std::move(tasks)));
            };
            ths[i] = std::thread(th, rdma_clis[i], i);
        }
        auto start = std::chrono::steady_clock::now();
        start_flag = true ;
        for (uint64_t i = 0; i < config.num_cli; i++)
        {
            ths[i].join();
        }
        auto end = std::chrono::steady_clock::now();
        double op_cnt = 1.0 * load_num;
        double duration = std::chrono::duration<double, std::milli>(end - start).count();
        printf("Load duration:%.2lfms\n", duration);
        printf("Load IOPS:%.2lfKops\n", op_cnt / duration);
        fflush(stdout); 

        printf("Run start\n");
        start_flag = false ;
        op_counter.store( 0 ) ;
        auto op_per_coro = config.num_op / (config.num_machine * config.num_cli * config.num_coro);
        std::vector<Generator *> gens;
        for (uint64_t i = 0; i < config.num_cli * config.num_coro; i++)
        {
            if (config.pattern_type == 0)
            {
                gens.push_back(new seq_gen(op_per_coro));
            }
            else if (config.pattern_type == 1)
            {
                gens.push_back(new uniform(op_per_coro));
            }
            else if (config.pattern_type == 2)
            {
                gens.push_back(new zipf99(op_per_coro));
            }
            else
            {
                gens.push_back(new SkewedLatestGenerator(op_per_coro));
            }
        }
        for (uint64_t i = 0; i < config.num_cli; i++)
        {
            auto th = [&](rdma_client *rdma_cli, uint64_t cli_id) {
                std::vector<task<>> tasks;
                for (uint64_t j = 0; j < config.num_coro; j++)
                {
                    tasks.emplace_back(run(gens[cli_id * config.num_coro + j],
                                           (ClientType *)(clis[cli_id * config.num_coro + j]), cli_id, j));
                }
                rdma_cli->run(gather(std::move(tasks)));
            };
            ths[i] = std::thread(th, rdma_clis[i], i);
        }
        start = std::chrono::steady_clock::now();
        start_flag = true ;
        for (uint64_t i = 0; i < config.num_cli; i++)
        {
            ths[i].join();
        }
        end = std::chrono::steady_clock::now();
        op_cnt = 1.0 * config.num_op;
        duration = std::chrono::duration<double, std::milli>(end - start).count();
        printf("Run duration:%.2lfms\n", duration);
        printf("Run IOPS:%.3lfMops\n", op_cnt / duration / 1000.0);
        fflush(stdout);

        exit_flag.store(false);

        for (auto gen : gens)
        {
            delete gen;
        }

        // Reset Ser
        if (config.machine_id == 0)
        {
            rdma_clis[0]->run(((ClientType *)clis[0])->reset_remote());
        }

        free(mem_buf);
        for (uint64_t i = 0; i < config.num_cli; i++)
        {
            for (uint64_t j = 0; j < config.num_coro; j++)
            {
                // rdma_free_mr(lmrs[i * config.num_coro + j], false);
                delete clis[i * config.num_coro + j];
            }
            delete rdma_wowait_conns[i];
            delete rdma_conns[i];
            delete rdma_clis[i];
        }
    }
}
