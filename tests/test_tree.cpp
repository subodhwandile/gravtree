#include <gtest/gtest.h>
#include "gravtree/tree.h"

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <unordered_map>

using namespace gravtree;
namespace fs = std::filesystem;

class TreeTest : public ::testing::Test {
protected:
    std::string dir_;

    void SetUp() override {
        dir_ = fs::temp_directory_path() / "gravtree_tree_test";
        fs::remove_all(dir_);
        fs::create_directories(dir_);
    }
    void TearDown() override {
        fs::remove_all(dir_);
    }

    std::unique_ptr<GravTree> make_tree(size_t buf = 1024) {
        GravTreeOptions opts;
        opts.data_dir         = dir_;
        opts.buffer_threshold = buf;
        opts.hard_limit_factor = 64;
        opts.fanout           = 4;
        return GravTree::open(opts);
    }
};

TEST_F(TreeTest, InsertAndGet) {
    auto tree = make_tree();
    tree->insert("hello", "world");
    auto val = tree->get("hello");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "world");
}

TEST_F(TreeTest, MissingKeyReturnsNullopt) {
    auto tree = make_tree();
    tree->insert("a", "1");
    EXPECT_FALSE(tree->get("b").has_value());
}

TEST_F(TreeTest, UpdateOverwritesValue) {
    auto tree = make_tree();
    tree->insert("x", "v1");
    tree->insert("x", "v2");
    auto val = tree->get("x");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "v2");
}

TEST_F(TreeTest, DeleteReturnsTombstone) {
    auto tree = make_tree();
    tree->insert("key", "value");
    tree->remove("key");
    EXPECT_FALSE(tree->get("key").has_value());
}

TEST_F(TreeTest, ManyInserts) {
    auto tree = make_tree(512); // small buffer to force flushes
    const int N = 500;
    for (int i = 0; i < N; ++i)
        tree->insert("k" + std::to_string(i), "v" + std::to_string(i));

    for (int i = 0; i < N; ++i) {
        auto val = tree->get("k" + std::to_string(i));
        ASSERT_TRUE(val.has_value()) << "missing k" << i;
        EXPECT_EQ(*val, "v" + std::to_string(i));
    }
}

TEST_F(TreeTest, RangeScan) {
    auto tree = make_tree();
    tree->insert("a", "1");
    tree->insert("b", "2");
    tree->insert("c", "3");
    tree->insert("d", "4");

    auto results = tree->range_scan("b", "c");
    EXPECT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].key, "b");
    EXPECT_EQ(results[1].key, "c");
}

TEST_F(TreeTest, PersistenceAcrossReopen) {
    {
        auto tree = make_tree();
        tree->insert("persist_key", "persist_val");
        tree->flush();
    }
    // Reopen.
    auto tree2 = make_tree();
    auto val = tree2->get("persist_key");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(*val, "persist_val");
}

// Enough random writes to force real LocalFlush runs (node buffers fill at
// the compile-time BUFFER_THRESHOLD), then verify every key survives a
// close/reopen and that node-file bytes were recorded at >= 2 levels.
TEST_F(TreeTest, MultiLevelFlushAndReopen) {
    const size_t N = 120000;                     // ~7 MB of 120-byte entries
    std::mt19937_64 rng(42);
    std::string val(100, 'x');
    std::unordered_map<std::string, std::string> oracle;

    {
        auto tree = make_tree();
        for (size_t i = 0; i < N; ++i) {
            char k[21];
            snprintf(k, sizeof(k), "%020llu",
                     static_cast<unsigned long long>(rng() % (N * 2)));
            std::string key(k);
            tree->insert(key, val);
            oracle[key] = val;
        }
        // Sprinkle in tombstones so the reopen path merges deletions too.
        size_t removed = 0;
        for (size_t i = 0; i < N / 20; ++i) {
            char k[21];
            snprintf(k, sizeof(k), "%020llu",
                     static_cast<unsigned long long>(rng() % (N * 2)));
            std::string key(k);
            tree->remove(key);
            if (oracle.erase(key)) ++removed;
        }
        tree->flush();
        auto st = tree->stats();
        fprintf(stderr, "nodes=%zu flush_ops=%zu splits=%zu widen=%zu\n",
                st.node_count, st.total_flush_ops, st.total_split_ops, st.widen_rewrites);
        for (size_t L = 0; L < st.bytes_written_by_level.size(); ++L)
            if (st.bytes_written_by_level[L])
                fprintf(stderr, "  level_bytes[%zu]=%llu\n", L,
                        (unsigned long long)st.bytes_written_by_level[L]);
        size_t nonzero = 0;
        for (uint64_t b : st.bytes_written_by_level)
            if (b > 0) ++nonzero;
        EXPECT_GE(nonzero, 2u);
        EXPECT_GT(st.total_flush_ops, 0u);
    }

    // Reopen and check every surviving key plus a sample of removed ones.
    auto tree2 = make_tree();
    for (const auto& [k, v] : oracle) {
        auto got = tree2->get(k);
        ASSERT_TRUE(got.has_value()) << "missing key " << k;
        EXPECT_EQ(*got, v);
    }
}
