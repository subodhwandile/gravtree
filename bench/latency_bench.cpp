// latency_bench.cpp
// Head-to-head latency comparison: GravTree vs RocksDB
//
// Runs four workloads on both engines and prints:
//   - Percentile table (p50 / p99 / p99.9 / p99.99 / max)
//   - Latency histogram
//   - Time-series (max latency per 2000-op window) — shows compaction stalls
//
// This is the core evaluation section of the GravTree paper.
// Key claim: GravTree's local compaction eliminates the stall spikes
// visible in RocksDB's time-series under sustained write load.

#include "gravtree/tree.h"

// RocksDB headers
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace gravtree;
using Clock = std::chrono::high_resolution_clock;
using ns    = std::chrono::nanoseconds;

// ── helpers ───────────────────────────────────────────────────────────────────

static std::string padded_key(uint64_t i, size_t w = 20) {
    std::string s = std::to_string(i);
    return std::string(w - std::min(w, s.size()), '0') + s;
}

static std::string make_value(size_t sz = 100) {
    return std::string(sz, 'x');
}

// ── GravTree factory ──────────────────────────────────────────────────────────

static std::unique_ptr<GravTree> open_gravtree(const std::string& dir) {
    fs::remove_all(dir);
    GravTreeOptions opts;
    opts.data_dir         = dir;
    opts.buffer_threshold = 4ULL * 1024 * 1024;   // 4 MB
    opts.fanout           = 8;
    return GravTree::open(opts);
}

// ── RocksDB factory ───────────────────────────────────────────────────────────

// Returns unique_ptr — compatible with RocksDB 11.x new API.
static std::unique_ptr<rocksdb::DB> open_rocksdb(const std::string& dir) {
    fs::remove_all(dir);
    // RocksDB does not create missing parent directories.
    fs::create_directories(dir);
    rocksdb::Options opts;
    opts.create_if_missing          = true;
    opts.compression                = rocksdb::kNoCompression;
    opts.write_buffer_size          = 4ULL * 1024 * 1024;
    opts.max_write_buffer_number    = 3;
    opts.max_background_compactions = 1;
    opts.max_background_flushes     = 1;

    std::unique_ptr<rocksdb::DB> db;
    rocksdb::Status s = rocksdb::DB::Open(opts, dir, &db);
    assert(s.ok() && "RocksDB open failed");
    return db;
}

// ── statistics ────────────────────────────────────────────────────────────────

struct Stats {
    double p50, p90, p99, p999, p9999, mean, max_v;
    size_t count;
    double throughput_mops;   // million ops/sec
};

static Stats compute(std::vector<uint64_t>& v, double elapsed_sec = 0.0) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    auto p = [&](double pct) {
        size_t i = static_cast<size_t>(pct * n);
        return static_cast<double>(v[std::min(i, n - 1)]);
    };
    double sum = std::accumulate(v.begin(), v.end(), 0.0);
    double tput = elapsed_sec > 0 ? (n / elapsed_sec / 1e6) : 0.0;
    return {p(.50), p(.90), p(.99), p(.999), p(.9999),
            sum / n, static_cast<double>(v.back()), n, tput};
}

static void print_row(const std::string& label, Stats& s,
                      const std::string& extra = "") {
    auto us = [](double x) { return x / 1000.0; };
    std::cout << std::left  << std::setw(18) << label
              << std::right << std::fixed << std::setprecision(2)
              << " | p50="    << std::setw(8) << us(s.p50)
              << " | p99="    << std::setw(8) << us(s.p99)
              << " | p99.9="  << std::setw(8) << us(s.p999)
              << " | p99.99=" << std::setw(9) << us(s.p9999)
              << " | max="    << std::setw(10) << us(s.max_v)
              << "  (μs)";
    if (s.throughput_mops > 0)
        std::cout << "  tput=" << std::setprecision(3) << s.throughput_mops << "M ops/s";
    if (!extra.empty()) std::cout << "  " << extra;
    std::cout << "\n";
}

static void print_histogram(const std::string& label,
                             const std::vector<uint64_t>& v) {
    const uint64_t bounds[] = {1'000, 10'000, 100'000,
                                1'000'000, 10'000'000};
    const char* names[] = {"  <1μs", " 1-10μs", "10-100μs",
                            "0.1-1ms", "  1-10ms", "  >10ms"};
    size_t cnt[6]{};
    for (uint64_t x : v) {
        int b = 5;
        for (int i = 0; i < 5; ++i) if (x < bounds[i]) { b = i; break; }
        cnt[b]++;
    }
    std::cout << "  Histogram [" << label << "]\n";
    for (int i = 0; i < 6; ++i) {
        double pct = 100.0 * cnt[i] / v.size();
        int bar = static_cast<int>(pct / 2);
        std::cout << "  " << names[i] << " | "
                  << std::string(bar, '#')
                  << " " << std::fixed << std::setprecision(1) << pct << "%\n";
    }
}

// time-series: print max latency per window — reveals compaction stall spikes
static void print_timeseries(const std::string& label,
                              const std::vector<uint64_t>& v,
                              size_t window = 2000) {
    std::cout << "  Time-series [" << label
              << "]  (peak latency per " << window << "-op window)\n";
    for (size_t i = 0; i < v.size(); i += window) {
        size_t end = std::min(i + window, v.size());
        uint64_t mx = *std::max_element(v.begin() + i, v.begin() + end);
        double us   = mx / 1000.0;
        int    bar  = std::min(static_cast<int>(us / 50), 60);
        std::cout << "  " << std::setw(8) << i << "  "
                  << std::string(bar, '|')
                  << " " << std::fixed << std::setprecision(1) << us << "μs\n";
    }
}

// ── workload runners ──────────────────────────────────────────────────────────

// Returns {latencies, elapsed_seconds}
using BenchResult = std::pair<std::vector<uint64_t>, double>;

// --- GravTree ---

static BenchResult gt_seq_write(size_t n, const std::string& dir) {
    auto tree = open_gravtree(dir);
    const std::string val = make_value();
    std::vector<uint64_t> lats; lats.reserve(n);
    auto wall0 = Clock::now();
    for (size_t i = 0; i < n; ++i) {
        auto t0 = Clock::now();
        tree->insert(padded_key(i), val);
        lats.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<ns>(Clock::now() - t0).count()));
    }
    tree->flush();
    double elapsed = std::chrono::duration<double>(Clock::now() - wall0).count();
    tree.reset();
    fs::remove_all(dir);
    return {std::move(lats), elapsed};
}

static BenchResult gt_rand_write(size_t n, const std::string& dir) {
    auto tree = open_gravtree(dir);
    std::mt19937_64 rng(42);
    const std::string val = make_value();
    std::vector<uint64_t> lats; lats.reserve(n);
    auto wall0 = Clock::now();
    for (size_t i = 0; i < n; ++i) {
        auto t0 = Clock::now();
        tree->insert(padded_key(rng() % (n * 2)), val);
        lats.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<ns>(Clock::now() - t0).count()));
    }
    tree->flush();
    double elapsed = std::chrono::duration<double>(Clock::now() - wall0).count();
    tree.reset();
    fs::remove_all(dir);
    return {std::move(lats), elapsed};
}

static BenchResult gt_point_read(size_t fill, size_t n,
                                  const std::string& dir) {
    auto tree = open_gravtree(dir);
    const std::string val = make_value();
    for (size_t i = 0; i < fill; ++i) tree->insert(padded_key(i), val);
    tree->flush();

    std::mt19937_64 rng(7);
    std::vector<uint64_t> lats; lats.reserve(n);
    auto wall0 = Clock::now();
    for (size_t i = 0; i < n; ++i) {
        std::string k = padded_key(rng() % fill);
        auto t0 = Clock::now();
        auto v  = tree->get(k); (void)v;
        lats.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<ns>(Clock::now() - t0).count()));
    }
    double elapsed = std::chrono::duration<double>(Clock::now() - wall0).count();
    tree.reset();
    fs::remove_all(dir);
    return {std::move(lats), elapsed};
}

static BenchResult gt_mixed(size_t n, const std::string& dir) {
    auto tree = open_gravtree(dir);
    std::mt19937_64 rng(99);
    const std::string val = make_value();
    for (size_t i = 0; i < n / 2; ++i) tree->insert(padded_key(i), val);

    std::vector<uint64_t> lats; lats.reserve(n);
    uint64_t wc = n / 2;
    auto wall0 = Clock::now();
    for (size_t i = 0; i < n; ++i) {
        auto t0 = Clock::now();
        if (rng() % 2 == 0) tree->insert(padded_key(wc++), val);
        else { auto v = tree->get(padded_key(rng() % (n/2))); (void)v; }
        lats.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<ns>(Clock::now() - t0).count()));
    }
    tree->flush();
    double elapsed = std::chrono::duration<double>(Clock::now() - wall0).count();
    tree.reset();
    fs::remove_all(dir);
    return {std::move(lats), elapsed};
}

// --- RocksDB ---

static BenchResult rdb_seq_write(size_t n, const std::string& dir) {
    auto db = open_rocksdb(dir);
    const std::string val = make_value();
    rocksdb::WriteOptions wo;
    wo.disableWAL = true;   // match GravTree: no WAL, same durability guarantee
    std::vector<uint64_t> lats; lats.reserve(n);
    auto wall0 = Clock::now();
    for (size_t i = 0; i < n; ++i) {
        auto t0 = Clock::now();
        db->Put(wo, padded_key(i), val);
        lats.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<ns>(Clock::now() - t0).count()));
    }
    db->Flush(rocksdb::FlushOptions{});
    double elapsed = std::chrono::duration<double>(Clock::now() - wall0).count();
    db.reset();
    fs::remove_all(dir);
    return {std::move(lats), elapsed};
}

static BenchResult rdb_rand_write(size_t n, const std::string& dir) {
    auto db = open_rocksdb(dir);
    std::mt19937_64 rng(42);
    const std::string val = make_value();
    rocksdb::WriteOptions wo;
    wo.disableWAL = true;   // match GravTree: no WAL, same durability guarantee
    std::vector<uint64_t> lats; lats.reserve(n);
    auto wall0 = Clock::now();
    for (size_t i = 0; i < n; ++i) {
        auto t0 = Clock::now();
        db->Put(wo, padded_key(rng() % (n * 2)), val);
        lats.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<ns>(Clock::now() - t0).count()));
    }
    db->Flush(rocksdb::FlushOptions{});
    double elapsed = std::chrono::duration<double>(Clock::now() - wall0).count();
    db.reset();
    fs::remove_all(dir);
    return {std::move(lats), elapsed};
}

static BenchResult rdb_point_read(size_t fill, size_t n,
                                   const std::string& dir) {
    auto db = open_rocksdb(dir);
    const std::string val = make_value();
    rocksdb::WriteOptions wo;
    wo.disableWAL = true;   // match GravTree: no WAL, same durability guarantee
    for (size_t i = 0; i < fill; ++i) db->Put(wo, padded_key(i), val);
    db->Flush(rocksdb::FlushOptions{});

    std::mt19937_64 rng(7);
    rocksdb::ReadOptions ro;
    std::vector<uint64_t> lats; lats.reserve(n);
    auto wall0 = Clock::now();
    for (size_t i = 0; i < n; ++i) {
        std::string k = padded_key(rng() % fill);
        std::string v;
        auto t0 = Clock::now();
        db->Get(ro, k, &v);
        lats.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<ns>(Clock::now() - t0).count()));
    }
    double elapsed = std::chrono::duration<double>(Clock::now() - wall0).count();
    db.reset();
    fs::remove_all(dir);
    return {std::move(lats), elapsed};
}

static BenchResult rdb_mixed(size_t n, const std::string& dir) {
    auto db = open_rocksdb(dir);
    std::mt19937_64 rng(99);
    const std::string val = make_value();
    rocksdb::WriteOptions wo;
    wo.disableWAL = true;   // match GravTree: no WAL, same durability guarantee
    rocksdb::ReadOptions ro;
    for (size_t i = 0; i < n / 2; ++i) db->Put(wo, padded_key(i), val);

    std::vector<uint64_t> lats; lats.reserve(n);
    uint64_t wc = n / 2;
    auto wall0 = Clock::now();
    for (size_t i = 0; i < n; ++i) {
        auto t0 = Clock::now();
        if (rng() % 2 == 0) {
            db->Put(wo, padded_key(wc++), val);
        } else {
            std::string v;
            db->Get(ro, padded_key(rng() % (n/2)), &v);
        }
        lats.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<ns>(Clock::now() - t0).count()));
    }
    db->Flush(rocksdb::FlushOptions{});
    double elapsed = std::chrono::duration<double>(Clock::now() - wall0).count();
    db.reset();
    fs::remove_all(dir);
    return {std::move(lats), elapsed};
}

// ── print comparison block ────────────────────────────────────────────────────

static void compare(const std::string& workload,
                    BenchResult gt_res,
                    BenchResult rdb_res) {
    auto& [gt_lats,  gt_elapsed]  = gt_res;
    auto& [rdb_lats, rdb_elapsed] = rdb_res;

    auto gt_s  = compute(gt_lats,  gt_elapsed);
    auto rdb_s = compute(rdb_lats, rdb_elapsed);

    std::cout << "\n╔═══ " << workload
              << " (" << gt_s.count << " ops) ═══╗\n";
    std::cout << "  (all latencies in μs)\n\n";

    // Percentile + throughput table
    std::cout << "  " << std::string(95, '-') << "\n";
    print_row("  GravTree",  gt_s);
    print_row("  RocksDB",   rdb_s);
    std::cout << "  " << std::string(95, '-') << "\n";

    // Improvement ratios
    auto ratio = [](double a, double b) {
        if (a <= b) {
            std::cout << "  GravTree is " << std::fixed << std::setprecision(1)
                      << b/a << "× faster at that percentile\n";
        } else {
            std::cout << "  RocksDB is " << std::fixed << std::setprecision(1)
                      << a/b << "× faster at that percentile\n";
        }
    };
    std::cout << "  p99    → "; ratio(gt_s.p99,   rdb_s.p99);
    std::cout << "  p99.9  → "; ratio(gt_s.p999,  rdb_s.p999);
    std::cout << "  max    → "; ratio(gt_s.max_v, rdb_s.max_v);

    // Histograms
    std::cout << "\n";
    print_histogram("GravTree",  gt_lats);
    print_histogram("RocksDB",   rdb_lats);

    // Time-series (the key visual for the paper)
    // window = N/500 so we always get ~500 rows regardless of N
    size_t window = std::max<size_t>(gt_lats.size() / 500, 1000);
    std::cout << "\n";
    print_timeseries("GravTree", gt_lats,  window);
    std::cout << "\n";
    print_timeseries("RocksDB",  rdb_lats, window);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    // Allow overriding N from command line: ./gravtree_latency 1000000
    const size_t N = (argc > 1) ? std::stoull(argv[1]) : 10'000'000;
    const std::string base = "/tmp/gt_bench";

    std::cout << "╔══════════════════════════════════════════════════════╗\n";
    std::cout << "║     GravTree  vs  RocksDB  —  Latency Comparison    ║\n";
    std::cout << "║  N = " << std::setw(10) << N << " ops/workload, Release build  ║\n";
    std::cout << "╚══════════════════════════════════════════════════════╝\n";
    std::cout << "\nNote: Time-series rows show peak latency per window.\n"
              << "      A spike in the RocksDB row = compaction stall.\n"
              << "      GravTree should show a smooth profile with no spikes.\n";

    // ── 1. Sequential writes ─────────────────────────────────────────────────
    std::cout << "\n[1/4] Sequential writes (" << N << " ops)…\n";
    compare("Sequential Write",
            gt_seq_write(N,  base + "/gt_seqw"),
            rdb_seq_write(N, base + "/rdb_seqw"));

    // ── 2. Random writes ─────────────────────────────────────────────────────
    std::cout << "\n[2/4] Random writes (" << N << " ops)…\n";
    compare("Random Write",
            gt_rand_write(N,  base + "/gt_randw"),
            rdb_rand_write(N, base + "/rdb_randw"));

    // ── 3. Point reads ───────────────────────────────────────────────────────
    std::cout << "\n[3/4] Point reads (fill=" << N/10 << ", queries=" << N << ")…\n";
    compare("Point Read",
            gt_point_read(N/10, N,  base + "/gt_read"),
            rdb_point_read(N/10, N, base + "/rdb_read"));

    // ── 4. Mixed 50/50 ───────────────────────────────────────────────────────
    std::cout << "\n[4/4] Mixed 50/50 read-write (" << N << " ops)…\n";
    compare("Mixed 50/50",
            gt_mixed(N,  base + "/gt_mixed"),
            rdb_mixed(N, base + "/rdb_mixed"));

    std::cout << "\n✓ Done.\n";
    return 0;
}
