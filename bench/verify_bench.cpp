// verify_bench.cpp — correctness checker for GravTree
// Inserts N sequential keys, flushes, then verifies every key via get().
// Reopens the tree and re-verifies to test recovery from manifest.
// Prints stats (root_buf_bytes, flush_ops, split_ops) and on-disk usage.

#include "gravtree/tree.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

namespace fs = std::filesystem;
using namespace gravtree;
using Clock = std::chrono::steady_clock;

static std::string padded_key(uint64_t i, size_t w = 20) {
    std::string s = std::to_string(i);
    return std::string(w - std::min(w, s.size()), '0') + s;
}

static std::string make_value(size_t sz = 100) {
    return std::string(sz, 'x');
}

static std::unique_ptr<GravTree> open_gravtree(const std::string& dir) {
    fs::remove_all(dir);
    GravTreeOptions opts;
    opts.data_dir         = dir;
    opts.buffer_threshold = 4ULL * 1024 * 1024;   // 4 MB
    opts.fanout           = 8;
    return GravTree::open(opts);
}

// Reopen helper: same options, no remove_all.
static std::unique_ptr<GravTree> reopen_gravtree(const std::string& dir) {
    GravTreeOptions opts;
    opts.data_dir         = dir;
    opts.buffer_threshold = 4ULL * 1024 * 1024;
    opts.fanout           = 8;
    return GravTree::open(opts);
}

static void check_all(GravTree* tree, size_t N, const std::string& expected,
                      const char* label) {
    size_t missing = 0, mismatched = 0;
    for (size_t i = 0; i < N; ++i) {
        auto v = tree->get(padded_key(i));
        if (!v.has_value())      ++missing;
        else if (*v != expected) ++mismatched;
    }
    std::cout << label << " missing=" << missing
              << " mismatched=" << mismatched << "\n";
}

int main(int argc, char** argv) {
    const size_t N = argc > 1 ? std::stoull(argv[1]) : 1'000'000;
    const std::string dir = "/tmp/gt_verify";
    const std::string val = make_value();
    const auto t0 = Clock::now();

    {
        auto tree = open_gravtree(dir);
        for (size_t i = 0; i < N; ++i) tree->insert(padded_key(i), val);
        tree->flush();

        check_all(tree.get(), N, val, "[fresh]");

        auto st = tree->stats();
        uintmax_t files = 0, bytes = 0;
        for (auto& e : fs::recursive_directory_iterator(dir)) {
            if (e.is_regular_file()) { ++files; bytes += e.file_size(); }
        }
        double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        std::cout << "N=" << N
                  << " files=" << files
                  << " bytes=" << bytes
                  << " root_buf_bytes=" << st.root_buf_bytes
                  << " flush_ops=" << st.total_flush_ops
                  << " split_ops=" << st.total_split_ops
                  << " node_count=" << st.node_count
                  << " tree_height=" << st.tree_height
                  << " elapsed_s=" << secs << "\n";
    } // close/destroy first instance

    {
        auto tree = reopen_gravtree(dir);
        check_all(tree.get(), N, val, "[reopen]");
    }

    return 0;
}
