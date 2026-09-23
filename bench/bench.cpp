#include <benchmark/benchmark.h>
#include "gravtree/tree.h"

#include <filesystem>
#include <random>
#include <string>

using namespace gravtree;
namespace fs = std::filesystem;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string rand_key(std::mt19937& rng, size_t len = 16) {
    static const char alpha[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::string s(len, 'x');
    for (auto& c : s) c = alpha[rng() % (sizeof(alpha) - 1)];
    return s;
}

static std::unique_ptr<GravTree> open_tree(const std::string& dir,
                                           size_t buf = BUFFER_THRESHOLD) {
    fs::remove_all(dir);
    GravTreeOptions opts;
    opts.data_dir         = dir;
    opts.buffer_threshold = buf;
    opts.fanout           = 8;
    return GravTree::open(opts);
}

// ── Sequential write ──────────────────────────────────────────────────────────

static void BM_SequentialWrite(benchmark::State& state) {
    std::string dir = "/tmp/gravtree_bench_seqwrite";
    auto tree = open_tree(dir);
    uint64_t key_counter = 0;
    for (auto _ : state) {
        std::string k = "key_" + std::to_string(key_counter++);
        tree->insert(k, "value_payload_xxxxxxxxxxxxxxxxxx");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SequentialWrite)->Iterations(100'000);

// ── Random write ─────────────────────────────────────────────────────────────

static void BM_RandomWrite(benchmark::State& state) {
    std::string dir = "/tmp/gravtree_bench_randwrite";
    auto tree = open_tree(dir);
    std::mt19937 rng(42);
    for (auto _ : state) {
        tree->insert(rand_key(rng), "value_payload_xxxxxxxxxxxxxxxxxx");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RandomWrite)->Iterations(100'000);

// ── Point read (warm cache) ───────────────────────────────────────────────────

static void BM_PointRead(benchmark::State& state) {
    std::string dir = "/tmp/gravtree_bench_read";
    auto tree = open_tree(dir);
    const int FILL = 10'000;
    for (int i = 0; i < FILL; ++i)
        tree->insert("rkey_" + std::to_string(i), "v" + std::to_string(i));

    std::mt19937 rng(7);
    for (auto _ : state) {
        std::string k = "rkey_" + std::to_string(rng() % FILL);
        benchmark::DoNotOptimize(tree->get(k));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_PointRead)->Iterations(200'000);

// ── Mixed 50/50 read-write ────────────────────────────────────────────────────

static void BM_Mixed(benchmark::State& state) {
    std::string dir = "/tmp/gravtree_bench_mixed";
    auto tree = open_tree(dir);
    const int FILL = 5'000;
    for (int i = 0; i < FILL; ++i)
        tree->insert("mkey_" + std::to_string(i), "v" + std::to_string(i));

    std::mt19937 rng(99);
    uint64_t wc = FILL;
    for (auto _ : state) {
        if (rng() % 2 == 0) {
            tree->insert("mkey_" + std::to_string(wc++), "new_value");
        } else {
            benchmark::DoNotOptimize(
                tree->get("mkey_" + std::to_string(rng() % FILL)));
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Mixed)->Iterations(100'000);
