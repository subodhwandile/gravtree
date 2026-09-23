#include <gtest/gtest.h>
#include "gravtree/node.h"

#include <filesystem>
#include <string>

using namespace gravtree;
namespace fs = std::filesystem;

class NodeTest : public ::testing::Test {
protected:
    std::string dir_;

    void SetUp() override {
        dir_ = fs::temp_directory_path() / "gravtree_node_test";
        fs::create_directories(dir_);
    }
    void TearDown() override {
        fs::remove_all(dir_);
    }
};

TEST_F(NodeTest, CreateAndLoad) {
    std::vector<Entry> entries;
    for (int i = 0; i < 100; ++i)
        entries.push_back({"key_" + std::to_string(i),
                           "val_" + std::to_string(i),
                           static_cast<uint64_t>(i), false});

    SSTNode node = SSTNode::create(42, 0, std::move(entries), {}, dir_);
    EXPECT_EQ(node.id, 42u);
    EXPECT_EQ(node.level, 0u);
    EXPECT_EQ(node.buffer.size(), 100u);
    EXPECT_FALSE(node.file_path.empty());
    EXPECT_TRUE(fs::exists(node.file_path));
}

TEST_F(NodeTest, FlushAndReloadEntries) {
    std::vector<Entry> entries;
    for (int i = 0; i < 50; ++i)
        entries.push_back({"item_" + std::to_string(i),
                           "data_" + std::to_string(i),
                           static_cast<uint64_t>(i + 1), false});

    SSTNode orig = SSTNode::create(1, 0, std::move(entries), {}, dir_);

    // Load only header.
    SSTNode loaded = SSTNode::load_header(orig.file_path);
    EXPECT_EQ(loaded.id, 1u);
    EXPECT_FALSE(loaded.is_loaded());

    // Now load entries.
    loaded.load_entries();
    EXPECT_TRUE(loaded.is_loaded());
    EXPECT_EQ(loaded.buffer.size(), 50u);
}

TEST_F(NodeTest, PointLookup) {
    std::vector<Entry> entries;
    for (int i = 0; i < 20; ++i)
        entries.push_back({"key" + std::to_string(i),
                           "v"   + std::to_string(i),
                           static_cast<uint64_t>(i + 1), false});

    SSTNode node = SSTNode::create(2, 0, std::move(entries), {}, dir_);

    auto found = node.get("key5");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->value, "v5");

    auto missing = node.get("nothere");
    EXPECT_FALSE(missing.has_value());
}

TEST_F(NodeTest, BloomFilterNegative) {
    std::vector<Entry> entries;
    entries.push_back({"alpha", "1", 1, false});
    entries.push_back({"beta",  "2", 2, false});

    SSTNode node = SSTNode::create(3, 0, std::move(entries), {}, dir_);
    // "gamma" was never inserted — bloom should not report it (mostly).
    // (Bloom can have FP, but it definitely cannot have FN.)
    EXPECT_TRUE(node.might_contain("alpha"));
    EXPECT_TRUE(node.might_contain("beta"));
}

TEST_F(NodeTest, ChildPointersPersist) {
    std::vector<NodeId> children = {10, 20, 30};
    SSTNode node = SSTNode::create(5, 1, {}, children, dir_);
    EXPECT_EQ(node.num_children, 3u);

    SSTNode reloaded = SSTNode::load_header(node.file_path);
    EXPECT_EQ(reloaded.num_children, 3u);
    EXPECT_EQ(reloaded.child_ids[0], 10u);
    EXPECT_EQ(reloaded.child_ids[1], 20u);
    EXPECT_EQ(reloaded.child_ids[2], 30u);
}
