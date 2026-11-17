// crash_bench: crash-recovery comparison.
//
//   crash_bench fill <gt|rdb> <dir> <n>   — insert n keys, then SIGKILL itself
//   crash_bench open <gt|rdb> <dir> <n>   — reopen, time open, count readable keys
//
// RocksDB runs with WAL ENABLED (its default durability mode): after a
// crash it replays the WAL on open. GravTree has no WAL — it reopens the
// last committed manifest; buffered writes since the last flush are lost.

#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <string>
#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include "gravtree/tree.h"

namespace fs = std::filesystem;
using namespace gravtree;
using Clock = std::chrono::steady_clock;

static std::string padded_key(uint64_t i) {
    char buf[21];
    std::snprintf(buf, sizeof(buf), "key%018llu", (unsigned long long)i);
    return std::string(buf);
}

static int do_fill_gt(const std::string& dir, uint64_t n) {
    GravTreeOptions opts;
    opts.data_dir = dir;
    opts.buffer_threshold = 4ULL * 1024 * 1024;
    auto tree = GravTree::open(opts);
    std::string val(100, 'v');
    for (uint64_t i = 0; i < n; ++i) tree->insert(padded_key(i), val);
    fsync(0);
    std::raise(SIGKILL);
    return 0;
}

static int do_open_gt(const std::string& dir, uint64_t n) {
    auto t0 = Clock::now();
    GravTreeOptions opts;
    opts.data_dir = dir;
    opts.buffer_threshold = 4ULL * 1024 * 1024;
    auto tree = GravTree::open(opts);
    double open_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    uint64_t hits = 0;
    for (uint64_t i = 0; i < n; ++i)
        if (tree->get(padded_key(i)).has_value()) ++hits;
    std::printf("gravtree reopen_ms=%.1f readable=%llu/%llu\n",
                open_ms, (unsigned long long)hits, (unsigned long long)n);
    return 0;
}

static int do_fill_rdb(const std::string& dir, uint64_t n) {
    fs::create_directories(dir);
    rocksdb::Options o;
    o.create_if_missing = true;
    o.compression = rocksdb::kNoCompression;
    o.write_buffer_size = 4ULL * 1024 * 1024;
    rocksdb::DB* db = nullptr;
    if (!rocksdb::DB::Open(o, dir, &db).ok()) return 1;
    std::string val(100, 'v');
    rocksdb::WriteOptions wo;   // WAL enabled — default durability
    for (uint64_t i = 0; i < n; ++i) db->Put(wo, padded_key(i), val);
    fsync(0);
    std::raise(SIGKILL);
    return 0;
}

static int do_open_rdb(const std::string& dir, uint64_t n) {
    rocksdb::Options o;
    o.create_if_missing = false;
    o.compression = rocksdb::kNoCompression;
    o.write_buffer_size = 4ULL * 1024 * 1024;
    auto t0 = Clock::now();
    rocksdb::DB* db = nullptr;
    rocksdb::Status s = rocksdb::DB::Open(o, dir, &db);
    double open_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    if (!s.ok()) { std::printf("rocksdb open FAILED: %s\n", s.ToString().c_str()); return 1; }
    uint64_t hits = 0;
    rocksdb::ReadOptions ro;
    std::string out;
    for (uint64_t i = 0; i < n; ++i)
        if (db->Get(ro, padded_key(i), &out).ok()) ++hits;
    std::printf("rocksdb reopen_ms=%.1f readable=%llu/%llu\n",
                open_ms, (unsigned long long)hits, (unsigned long long)n);
    delete db;
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: crash_bench <fill|open> <gt|rdb> <dir> <n>\n");
        return 2;
    }
    std::string mode = argv[1], eng = argv[2], dir = argv[3];
    uint64_t n = std::stoull(argv[4]);
    if (mode == "fill" && eng == "gt")  return do_fill_gt(dir, n);
    if (mode == "open" && eng == "gt")  return do_open_gt(dir, n);
    if (mode == "fill" && eng == "rdb") return do_fill_rdb(dir, n);
    if (mode == "open" && eng == "rdb") return do_open_rdb(dir, n);
    std::fprintf(stderr, "bad args\n");
    return 2;
}
