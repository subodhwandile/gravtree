#include <gtest/gtest.h>
#include "gravtree/flush.h"
#include "gravtree/manifest.h"

#include <filesystem>

using namespace gravtree;
namespace fs = std::filesystem;

class FlushTest : public ::testing::Test {
protected:
    std::string dir_;
    std::unique_ptr<Manifest> manifest_;

    void SetUp() override {
        dir_ = fs::temp_directory_path() / "gravtree_flush_test";
        fs::create_directories(dir_);
        manifest_ = std::make_unique<Manifest>(dir_);
    }
    void TearDown() override {
        fs::remove_all(dir_);
    }

    SSTNode make_leaf(NodeId id, const std::string& min_k,
                      const std::string& max_k,
                      std::vector<Entry> entries) {
        SSTNode n = SSTNode::create(id, 0, std::move(entries), {}, dir_);
        n.key_range.min_key = min_k;
        n.key_range.max_key = max_k;
        manifest_->register_node(id, 0, n.file_path);
        return n;
    }
};

TEST_F(FlushTest, PartitionEntriesCorrectly) {
    // Two children: child0 covers [a–m], child1 covers [n–z]
    SSTNode c0 = make_leaf(1, "a", "m", {});
    SSTNode c1 = make_leaf(2, "n", "z", {});
    std::vector<SSTNode> children{std::move(c0), std::move(c1)};

    std::vector<Entry> entries{
        {"apple", "1", 1, false},
        {"mango", "2", 2, false},
        {"orange", "3", 3, false},
        {"zebra", "4", 4, false},
    };

    auto buckets = partition_entries(entries, children);
    ASSERT_EQ(buckets.size(), 2u);
    EXPECT_EQ(buckets[0].size(), 2u); // apple, mango
    EXPECT_EQ(buckets[1].size(), 2u); // orange, zebra
}

TEST_F(FlushTest, MergePreservesNewest) {
    std::vector<Entry> a{{"key", "old", 1, false}};
    std::vector<Entry> b{{"key", "new", 5, false}};

    auto merged = merge_sorted_entries(a, b);
    ASSERT_EQ(merged.size(), 1u);
    EXPECT_EQ(merged[0].value, "new");
    EXPECT_EQ(merged[0].seq_no, 5u);
}

TEST_F(FlushTest, LocalFlushDistributesData) {
    // Parent has 4 entries; 2 children split at "m"
    std::vector<Entry> parent_buf{
        {"alpha", "1", 1, false},
        {"beta",  "2", 2, false},
        {"nano",  "3", 3, false},
        {"zeta",  "4", 4, false},
    };
    SSTNode parent = SSTNode::create(10, 1, {}, {1u, 2u}, dir_);
    parent.buffer  = parent_buf;
    manifest_->register_node(10, 1, parent.file_path);

    SSTNode c0 = make_leaf(1, "a", "m", {});
    SSTNode c1 = make_leaf(2, "n", "z", {});
    std::vector<SSTNode> children{std::move(c0), std::move(c1)};

    FlushResult res = local_flush(parent, children, *manifest_, dir_);

    EXPECT_EQ(res.new_child_ids.size(), 2u);

    // child0 should have alpha, beta; child1 should have nano, zeta
    children[0].load_entries();
    children[1].load_entries();
    EXPECT_EQ(children[0].buffer.size(), 2u);
    EXPECT_EQ(children[1].buffer.size(), 2u);
}
