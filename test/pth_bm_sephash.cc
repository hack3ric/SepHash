

#include <cstdint>
#include <cstdlib>
#include "aiordma.h"
#include "sephash.h"

extern "C" {
void *pth_bm_target_create() {
    Config config;
    const char *argv[] = {"ser_cli",
                          "--server_ip",
                          getenv("SEPHASH_SERVER_IP"),
                          "--num_machine",
                          "1",
                          "--num_cli",
                          "1",
                          "--num_coro",
                          "1",
                          "--gid_idx",
                          "1",
                          "--machine_id",
                          "0"};
    config.ParseArg(sizeof(argv) / sizeof(*argv), argv);

    uint64_t cbuf_size = (1ul << 20) * 250;
    char *mem_buf = (char *)malloc(cbuf_size * (config.num_cli * config.num_coro + 1));

    auto dev = new rdma_dev("mlx5_0", 1, config.gid_idx);
    auto rdma_cli = new rdma_client(*dev);
    std::cout << "sephash: memory server ip = " << config.server_ip << std::endl;
    auto rdma_conn = rdma_cli->connect(config.server_ip);
    assert(rdma_conn != nullptr);
    auto rdma_wowait_conn = rdma_cli->connect(config.server_ip);
    assert(rdma_wowait_conn != nullptr);
    ibv_mr *lmr = dev->create_mr(cbuf_size, mem_buf);

    auto cli = new SEPHASH::Client(config, lmr, rdma_cli, rdma_conn, rdma_wowait_conn,
                                   config.machine_id, 0, 0);
    return cli;
}

// void pth_bm_target_init_thread(void *target) {}

// void pth_bm_target_print_stat(void *target) {}

void pth_bm_target_destroy(void *target) {}

void pth_bm_target_read(void *target, int key) {
    auto cli = static_cast<SEPHASH::Client *>(target);
    SEPHASH::Slice key_, value_;
    key_.len = sizeof(key);
    key_.data = (char *)&key;
    cli->cli->run(cli->search(&key_, &value_));
}

void pth_bm_target_insert(void *target, int key) {
    auto cli = static_cast<SEPHASH::Client *>(target);
    uint64_t value = 0xdeadbeef;
    SEPHASH::Slice key_, value_;
    key_.len = sizeof(key);
    key_.data = (char *)&key;
    value_.len = sizeof(value);
    value_.data = (char *)&value;
    cli->cli->run(cli->insert(&key_, &value_));
}

void pth_bm_target_update(void *target, int key) {
    auto cli = static_cast<SEPHASH::Client *>(target);
    uint64_t value = 0xdeadcafe;
    SEPHASH::Slice key_, value_;
    key_.len = sizeof(key);
    key_.data = (char *)&key;
    value_.len = sizeof(value);
    value_.data = (char *)&value;
    cli->cli->run(cli->update(&key_, &value_));
}

void pth_bm_target_delete(void *target, int key) {
    auto cli = static_cast<SEPHASH::Client *>(target);
    SEPHASH::Slice key_;
    key_.len = sizeof(key);
    key_.data = (char *)&key;
    cli->cli->run(cli->remove(&key_));
}
}
