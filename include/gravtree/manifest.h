#pragma once

#include "gravtree/types.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gravtree {

// ── NodeMeta ─────────────────────────────────────────────────────────────────
// Lightweight record the Manifest keeps for every known node.

struct NodeMeta {
    NodeId      id        {INVALID_NODE_ID};
    uint32_t    level     {0};
    std::string file_path;
    bool        valid     {true};
};

// ── Manifest ─────────────────────────────────────────────────────────────────
// Tracks all SSTable node files on disk.  Every time a node is created,
// split, or flushed it gets a new NodeId and a new file; the old entry
// is invalidated and will be deleted during the next GC pass.
//
// The manifest is written atomically: new content goes to
// <dir>/manifest.tmp, then rename(2) replaces <dir>/manifest.
//
// Text format (one token-per-line, easy to inspect):
//   VERSION 1
//   ROOT    <node_id>
//   NEXT_ID <next_id>
//   NODE    <id> <level> <valid 0|1> <file_path>
//   ...

class Manifest {
public:
    explicit Manifest(const std::string& dir);

    // Load state from disk (called once on open).
    void load();

    // Persist current state atomically.
    void save() const;

    // Register a newly created node.
    void register_node(NodeId id, uint32_t level, const std::string& file_path);

    // Mark node as stale (pending GC).
    void invalidate_node(NodeId id);

    // Return file path for a valid node (throws if not found).
    std::string node_path(NodeId id) const;

    // All valid nodes at a given level.
    std::vector<NodeMeta> nodes_at_level(uint32_t level) const;

    // Root management.
    NodeId root_id()           const noexcept { return root_id_; }
    void   set_root(NodeId id);

    // Allocate the next unique NodeId.
    NodeId next_node_id() { return next_id_.fetch_add(1); }

    // Delete files for all invalid nodes.
    void gc();

private:
    std::string dir_;
    NodeId      root_id_{INVALID_NODE_ID};
    std::atomic<NodeId> next_id_{1};
    std::unordered_map<NodeId, NodeMeta> nodes_;
    mutable std::mutex mu_;

    std::string manifest_path() const;
    std::string tmp_path()      const;
};

} // namespace gravtree
