#pragma once
#include "aiordma.h"
#include "alloc.h"
#include "config.h"
#include "hash.h"
#include "kv_trait.h"
#include "perf.h"
#include "search.h"
#include <cassert>
#include <chrono>
#include <fcntl.h>
#include <map>
#include <math.h>
#include <tuple>
#include <vector>

// #define TOO_LARGE_KV


namespace SEPHASH
{
constexpr uint64_t SEGMENT_SIZE = 1024;
constexpr uint64_t SLOT_PER_SEG = ((SEGMENT_SIZE) / (sizeof(uint64_t)+sizeof(uint8_t)));
constexpr uint64_t SLOT_BATCH_SIZE = 8;
constexpr uint64_t RETRY_LIMIT = (SLOT_PER_SEG/SLOT_BATCH_SIZE); // TODO : 后期试试改成其他较小的值
constexpr uint64_t MAX_MAIN_SIZE = 64 * SLOT_PER_SEG;
constexpr uint64_t MAX_FP_INFO = 256;
constexpr uint64_t INIT_DEPTH = 4;
constexpr uint64_t MAX_DEPTH = 16;
constexpr uint64_t DIR_SIZE = (1 << MAX_DEPTH);
constexpr uint64_t ALIGNED_SIZE = 64;             // aligned size of len bitfield in DepSlot
constexpr uint64_t dev_mem_size = (1 << 10) * 64; // 64KB的dev mem，用作lock
constexpr uint64_t num_lock =
    (dev_mem_size - sizeof(uint64_t)) / sizeof(uint64_t); // Lock数量，client对seg_id使用hash来共享lock

struct Slot
{
    uint8_t fp : 8;
    uint8_t len : 3;
    uint8_t sign : 1; // 用来表示split delete信息
    uint8_t dep : 4;
    uint64_t offset : 48;
    uint8_t fp_2;
    operator uint64_t()
    {
        return *(uint64_t *)this;
    }
    Slot(uint64_t u)
    {
        *this = *(Slot *)(&u);
    }
    bool operator<(const Slot &a) const
    {
        return fp < a.fp;
    }
    void print(uint64_t slot_id = -1)
    {
        if(slot_id!=-1) printf("slot_id:%lu\t", slot_id);
        printf("fp:%x\t", fp);
        printf("fp_2:%x\t", fp_2);
        printf("len:%d\t", len);
        printf("sign:%d\t", sign);
        printf("dep:%d\t", dep);
        printf("offset:%lx\t", offset);
        printf("size:%ld\n", sizeof(Slot));
    }
}__attribute__((aligned(1)));

struct Slice
{
    uint64_t len;
    char *data;

    void print_var_kv(){
        printf("len:%lu\n",len);
        for(uint64_t i = 0 ; i < len/8 ; i++){
            printf("key[%lu]:%lu\t",i,*(uint64_t *)(data+8*i));
        }
        printf("\n");
    }
};

struct KVBlock
{
    uint64_t k_len;
    uint64_t v_len;
    char data[0]; // 变长数组，用来保证KVBlock空间上的连续性，便于RDMA操作
    void print(const char *desc = nullptr)
    {
        if (desc != nullptr)
            log_err("%s klen:%lu key:%lu vlen:%lu value:%s", desc, k_len, *(uint64_t *)data, v_len,
                    data + sizeof(uint64_t));
        else
            log_err("klen:%lu key:%lu vlen:%lu value:%s", k_len, *(uint64_t *)data, v_len, data + sizeof(uint64_t));
    }

    void print_var_kv(){
        printf("klen:%lu vlen:%lu\n",k_len,v_len);
        for(uint64_t i = 0 ; i < k_len/8 ; i++){
            printf("key[%lu]:%lu\t",i,*(uint64_t *)(data+8*i));
        }
        printf("\n");
    }
}__attribute__((aligned(1)));

template <typename Alloc>
    requires Alloc_Trait<Alloc, uint64_t>
KVBlock *InitKVBlock(Slice *key, Slice *value, Alloc *alloc)
{
    KVBlock *kv_block = (KVBlock *)alloc->alloc(2 * sizeof(uint64_t) + key->len + value->len);
    kv_block->k_len = key->len;
    kv_block->v_len = value->len;
    memcpy(kv_block->data, key->data, key->len);
    memcpy(kv_block->data + key->len, value->data, value->len);
    return kv_block;
}

struct CurSegMeta{
    uint8_t sign : 1; // 实际中的split_lock可以和sign、depth合并，这里为了不降rdma驱动版本就没有合并。
    uint64_t local_depth : 63;
    uintptr_t main_seg_ptr;
    uintptr_t main_seg_len;
    uint64_t fp_bitmap[16]; // 16*64 = 1024,代表10bits fp的出现情况；整个CurSeg大约会出现（1024/8=128）个FP，因此能极大的减少search对CurSeg的访问
}__attribute__((aligned(1)));

struct CurSeg
{
    uint64_t split_lock;
    CurSegMeta seg_meta;
    Slot slots[SLOT_PER_SEG];
}__attribute__((aligned(1)));

struct MainSeg
{
    Slot slots[0];
}__attribute__((aligned(1)));

struct FpInfo{ 
    uint8_t num; // 数量 
    operator uint64_t()
    {
        return *(uint64_t *)this;
    }
}__attribute__((aligned(1)));

struct DirEntry
{
    // TODO : 实际上只需要用5 bits，为了方便ptr统一48，所以这里仍保留16bits
    uint64_t local_depth ; 
    uintptr_t cur_seg_ptr ;
    uintptr_t main_seg_ptr ;
    uint64_t main_seg_len ;
    FpInfo fp[MAX_FP_INFO];
    bool operator==(const DirEntry &other) const
    {
        return cur_seg_ptr == other.cur_seg_ptr && main_seg_ptr == other.main_seg_ptr &&
               main_seg_len == other.main_seg_len;
    }
} __attribute__((aligned(1)));

struct Directory
{
    uint64_t global_depth;   // number of segment
    DirEntry segs[DIR_SIZE]; // Directory use MSB and is allocated enough space in advance.
    uint64_t start_cnt;      // 为多客户端同步保留的字段，不影响原有空间布局

    void print(){
        log_err("Global_Depth:%lu",global_depth);
        for(uint64_t i = 0 ; i < (1<<global_depth) ; i++){
            log_err("Entry %lx : local_depth:%lu cur_seg_ptr:%lx main_seg_ptr:%lx main_seg_lne:%lx",i,segs[i].local_depth,segs[i].cur_seg_ptr,segs[i].main_seg_ptr,segs[i].main_seg_len);
        }
    }
} __attribute__((aligned(1)));

struct SlotOffset
{
    // 记录每个CurSeg中上次insert访问到的slot offset
    bool sign;
    uint8_t offset; 
    uint64_t main_seg_ptr; 
} __attribute__((aligned(1)));


class Client : public BasicDB
{
  public:
    Client(Config &config, ibv_mr *_lmr, rdma_client *_cli, rdma_conn *_conn, rdma_conn *_wowait_conn,
           uint64_t _machine_id, uint64_t _cli_id, uint64_t _coro_id);

    Client(const Client &) = delete;

    ~Client();

    // Used for sync operation and test
    task<> start(uint64_t total);
    task<> stop();
    task<> reset_remote();
    task<> cal_utilization();

    task<> insert(Slice *key, Slice *value);
    task<std::tuple<uintptr_t, uint64_t>> search(Slice *key, Slice *value);
    task<> update(Slice *key, Slice *value);
    task<> remove(Slice *key);

  private:
    task<> sync_dir();
    task<uintptr_t> check_gd(uint64_t segloc,bool read_fp);

    task<> Split(uint64_t seg_loc, uintptr_t seg_ptr, CurSegMeta *old_seg_meta);
    task<> merge_insert(Slot *data, uint64_t len, Slot *old_seg, uint64_t old_seg_len, Slot *new_seg, uint64_t *res );
  
    task<> print_main_seg(uint64_t seg_loc,uintptr_t main_seg_ptr, uint64_t main_seg_len);

    // rdma structs
    rdma_client *cli;
    rdma_conn *conn;
    rdma_conn *wo_wait_conn;
    rdma_rmr seg_rmr;
    struct ibv_mr *lmr;

    Alloc alloc;
    RAlloc ralloc;

    CurSeg *cur_seg;
    MainSeg *new_main_seg;
    
    // Debug
    uint64_t machine_id;
    uint64_t cli_id;
    uint64_t coro_id;
    uint64_t key_num;
    uint64_t key_off;
    uint64_t kv_block_len;

    // Statistic
    Perf perf;
    SumCost sum_cost;
    uint64_t op_cnt;
    uint64_t miss_cnt;
    uint64_t retry_cnt;

    // Data part
    SlotOffset offset[DIR_SIZE] ; // 记录当前CurSeg中的freeslot开头？仅作参考，还是每个cli进行随机read
                        // 还是随机read吧，使用一个固定的序列？保存在本地，免得需要修改远端的。
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
    void Init();

    rdma_dev dev;
    rdma_server ser;
    struct ibv_mr *seg_mr;
    ibv_dm *lock_dm; // Locks for Segments
    ibv_mr *lock_mr;
    char *mem_buf;

    Alloc alloc;
    Directory *dir;
};

} // namespace SPLIT_HASH