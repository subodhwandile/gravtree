#include <gtest/gtest.h>
#include "gravtree/tree.h"

#include <filesystem>
#include <string>

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
