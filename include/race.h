#pragma once
#include "aiordma.h"
#include "alloc.h"
#include "config.h"
#include "hash.h"
#include "kv_trait.h"
#include "perf.h"
#include <cassert>
#include <chrono>
#include <fcntl.h>
#include <map>
#include <math.h>
#include <tuple>
#include <vector>

#define WO_WAIT_WRITE

namespace RACE
{

// size of race segment = 8*8*(1<<6)*3 = 12KB
constexpr uint64_t SLOT_PER_BUCKET = 8;
constexpr uint64_t BUCKET_BITS = 12;
constexpr uint64_t BUCKET_PER_SEGMENT = 1 << (BUCKET_BITS);
constexpr uint64_t INIT_DEPTH = 4;
constexpr uint64_t MAX_DEPTH = 18;
constexpr uint64_t DIR_SIZE = (1 << MAX_DEPTH);

struct Slot
{
    uint8_t fp : 8;
    uint8_t len : 8;
    uint64_t offset : 48;
} __attribute__((aligned(1)));

struct Slice
{
    uint64_t len;
    char *data;
};

struct KVBlock
{
    uint64_t k_len;
    uint64_t v_len;
    char data[0]; //变长数组，用来保证KVBlock空间上的连续性，便于RDMA操作
};

template <typename Alloc>
requires Alloc_Trait<Alloc, uint64_t> KVBlock *InitKVBlock(Slice *key, Slice *value, Alloc *alloc)
{
    KVBlock *kv_block = (KVBlock *)alloc->alloc(2 * sizeof(uint64_t) + key->len + value->len);
    kv_block->k_len = key->len;
    kv_block->v_len = value->len;
    memcpy(kv_block->data, key->data, key->len);
    memcpy(kv_block->data + key->len, value->data, value->len);
    return kv_block;
}

struct Bucket
{
    uint32_t local_depth;
    uint32_t suffix;
    Slot slots[SLOT_PER_BUCKET];
} __attribute__((aligned(1)));

struct Segment
{
    struct Bucket buckets[BUCKET_PER_SEGMENT * 3];
} __attribute__((aligned(1)));

struct DirEntry
{
    uint64_t split_lock;
    uintptr_t seg_ptr;
    uint64_t local_depth;
} __attribute__((aligned(1)));

struct Directory
{
    uint64_t resize_lock; //最后位为global-split lock，后续为local-split count
    uint64_t global_depth;
    struct DirEntry segs[DIR_SIZE]; // Directory use MSB and is allocated enough space in advance.
    uint64_t start_cnt;             // 为多客户端同步保留的字段，不影响原有空间布局
    void print(){
        log_err("Global_Depth:%lu",global_depth);
        for(uint64_t i = 0 ; i < (1<<global_depth) ; i++){
            log_err("Entry %lu : local_depth:%lu seg_ptr:%lx ",i,segs[i].local_depth,segs[i].seg_ptr);
        }
    }
} __attribute__((aligned(1)));

class Client : public BasicDB
{
  public:
    Client(Config &config, ibv_mr *_lmr, rdma_client *_cli, rdma_conn *_conn,rdma_conn *_wowait_conn, uint64_t _machine_id,
               uint64_t _cli_id, uint64_t _coro_id);

    Client(const Client &) = delete;

    ~Client();

    // Used for sync operation and test
    task<> start(uint64_t total);
    task<> stop();
    task<> reset_remote();
    task<> cal_utilization();

    task<> insert(Slice *key, Slice *value);
    task<std::tuple<uintptr_t, uint64_t>> search(Slice *key, Slice *value); // return slotptr
    task<> update(Slice *key, Slice *value);
    task<> remove(Slice *key);

  private:
    task<> sync_dir();
    task<std::tuple<uintptr_t, uint64_t>> search_on_resize(Slice *key, Slice *value);
    task<bool> search_bucket(Slice *key, Slice *value, uintptr_t &slot_ptr, uint64_t &slot, Bucket *buc_data,
                             uintptr_t bucptr_1, uintptr_t bucptr_2, uint64_t pattern_1);

    bool FindLessBucket(Bucket *buc1, Bucket *buc2);

    uintptr_t FindEmptySlot(Bucket *buc, uint64_t buc_idx, uintptr_t buc_ptr);

    bool IsCorrectBucket(uint64_t segloc, Bucket *buc, uint64_t pattern);

    task<int> Split(uint64_t seg_loc, uintptr_t seg_ptr, uint64_t local_depth, bool global_flag);

    // Global/Local并行的方式造成的等待冲突太高了，就使用简单的单个lock
    task<int> LockDir();
    task<> UnlockDir();
    task<int> SetSlot(uint64_t buc_ptr, uint64_t slot);
    task<> MoveData(uint64_t local_depth, uint64_t old_seg_ptr, uint64_t new_seg_ptr, Segment *seg, Segment *new_seg);

    // rdma structs
    rdma_client *cli;
    rdma_conn *conn;
    rdma_conn *wowait_conn;
    rdma_rmr rmr;
    struct ibv_mr *lmr;

    Alloc alloc;
    RAlloc ralloc;
    uint64_t machine_id;
    uint64_t cli_id;
    uint64_t coro_id;
    uint64_t op_key;

    // Statistic
    Perf perf;
    SumCost sum_cost;

    // Data part
    Directory *dir;
};

class ClientMultiShard : public BasicDB{
    const static uint32_t seed = 0x1b873593;

    Client *get_shard(Slice *key) {
        uint32_t pos = hash(key->data, key->len, seed);
        return client_list[pos % shards];
    }

public:
    ClientMultiShard(Config &config, rdma_dev &dev, char *mem_buf, uint64_t cbuf_size,
        rdma_client *_cli, uint64_t _machine_id, uint64_t _cli_id, uint64_t _coro_id) {
        rdma_conn *rdma_conns, *rdma_wowait_conns;
        shards = config.server_num;
        client_list.resize(shards);
        for (uint64_t i = 0; i < shards; i++) {
            rdma_conns = _cli->connect(config.server_ip[i]);
            assert(rdma_conns != nullptr);
            rdma_wowait_conns = _cli->connect(config.server_ip[i]);
            assert(rdma_wowait_conns != nullptr);
            ibv_mr *lmr = dev.create_mr(cbuf_size, mem_buf + cbuf_size * ((_cli_id * config.num_coro + _coro_id) * config.server_num + i));
            client_list[i] = 
                    new Client(config, lmr, _cli, rdma_conns, rdma_wowait_conns, _machine_id, _cli_id, _coro_id);
        }
    }

    ~ClientMultiShard() {
        for (auto &client: client_list) {
            delete client;
        }
    }

    ClientMultiShard(const ClientMultiShard&) = delete;

    task<> start(uint64_t total) {
        for (auto &client: client_list) {
            co_await client->start(total);
        }
    }

    task<> stop() {
        for (auto &client: client_list) {
            co_await client->stop();
        }
    }

    task<> reset_remote() {
        for (auto &client: client_list) {
            co_await client->reset_remote();
        }
    }

    task<> cal_utilization() {
        for (auto &client: client_list) {
            co_await client->cal_utilization();
        }
    }

    task<> insert(Slice *key, Slice *value) {
        co_await get_shard(key)->insert(key, value);
    }

    task<> search(Slice *key, Slice *value) {
        co_await get_shard(key)->search(key, value);
    }

    task<> update(Slice *key, Slice *value) {
        co_await get_shard(key)->update(key, value);
    }

    task<> remove(Slice *key) {
        co_await get_shard(key)->remove(key);
    }

private:
    int shards;
    std::vector<Client*> client_list;
};

class Server : public BasicDB
{
  public:
    Server(Config &config);
    ~Server();

  private:
    void Init(Directory *dir);

    rdma_dev dev;
    rdma_server ser;
    struct ibv_mr *lmr;
    char *mem_buf;

    Alloc alloc;
    Directory *dir;
};

} // namespace RACE