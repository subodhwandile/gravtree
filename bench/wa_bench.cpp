// wa_bench.cpp — write-amplification measurement, GravTree vs RocksDB.
// Runs one workload (seq|rand) for N ops with the same options as
// latency_bench.cpp, then reports bytes passed to write(2) by this process
// (/proc/self/io wchar) relative to the logical payload N*(key+value).
// Usage: gravtree_wa <gt|rdb> <seq|rand> [N]

#include "gravtree/tree.h"
#include <rocksdb/db.h>
#include <rocksdb/options.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <string>

namespace fs = std::filesystem;
using namespace gravtree;

static std::string padded_key(uint64_t i, size_t w = 20) {
    std::string s = std::to_string(i);
    return std::string(w - std::min(w, s.size()), '0') + s;
}
static std::string make_value(size_t sz = 100) { return std::string(sz, 'x'); }

struct IoCounters { uint64_t rchar = 0, wchar = 0, read_bytes = 0, write_bytes = 0; };
static IoCounters read_io() {
    IoCounters c; std::ifstream f("/proc/self/io"); std::string k; uint64_t v;
    while (f >> k >> v) {
        if (k == "rchar:") c.rchar = v; else if (k == "wchar:") c.wchar = v;
        else if (k == "read_bytes:") c.read_bytes = v; else if (k == "write_bytes:") c.write_bytes = v;
    }
    return c;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "usage: gravtree_wa <gt|rdb> <seq|rand> [N]\n"; return 2; }
    const std::string engine = argv[1], mode = argv[2];
    const size_t N = argc > 3 ? std::stoull(argv[3]) : 1'000'000;
    const std::string dir = "/tmp/gt_wa/" + engine + "_" + mode;
    fs::remove_all(dir); fs::create_directories(dir);
    const std::string val = make_value();
    std::mt19937_64 rng(42);
    auto key_at = [&](size_t i) { return mode == "rand" ? padded_key(rng() % (N * 2)) : padded_key(i); };

    // Directory size after close (live bytes).
    auto dir_bytes = [&]() { uint64_t b = 0; for (auto& e : fs::recursive_directory_iterator(dir)) if (e.is_regular_file()) b += e.file_size(); return b; };

    IoCounters before = read_io();
    auto t0 = std::chrono::steady_clock::now();
    if (engine == "gt") {
        GravTreeOptions opts; opts.data_dir = dir; opts.buffer_threshold = 4ULL << 20; opts.fanout = 8;
        auto tree = GravTree::open(opts);
        for (size_t i = 0; i < N; ++i) tree->insert(key_at(i), val);
        tree->flush();
        auto st = tree->stats();
        std::cout << "flush_ops=" << st.total_flush_ops << " split_ops=" << st.total_split_ops << "\n";
        tree.reset();
    } else {
        rocksdb::Options o; o.create_if_missing = true; o.compression = rocksdb::kNoCompression;
        o.write_buffer_size = 4ULL << 20; o.max_write_buffer_number = 3;
        o.max_background_compactions = 1; o.max_background_flushes = 1;
        std::unique_ptr<rocksdb::DB> db; rocksdb::Status s = rocksdb::DB::Open(o, dir, &db);
        if (!s.ok()) { std::cerr << s.ToString() << "\n"; return 1; }
        rocksdb::WriteOptions wo; wo.disableWAL = true;
        for (size_t i = 0; i < N; ++i) db->Put(wo, key_at(i), val);
        db->Flush(rocksdb::FlushOptions{});
        // Wait for already-scheduled background compactions so their I/O is counted.
        db->WaitForCompact(rocksdb::WaitForCompactOptions{});
        db.reset();
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    IoCounters after = read_io();

    const double logical = double(N) * (20 + 100);
    const double wchar = double(after.wchar - before.wchar);
    const double rchar = double(after.rchar - before.rchar);
    const double wb    = double(after.write_bytes - before.write_bytes);
    std::cout << engine << " " << mode << " N=" << N
              << " logical_bytes=" << uint64_t(logical)
              << " wchar=" << uint64_t(wchar) << " write_bytes=" << uint64_t(wb)
              << " rchar=" << uint64_t(rchar)
              << " live_bytes=" << dir_bytes()
              << " WA(wchar/logical)=" << wchar / logical
              << " RA(rchar/logical)=" << rchar / logical
              << " secs=" << secs << "\n";
    fs::remove_all(dir);
    return 0;
}
