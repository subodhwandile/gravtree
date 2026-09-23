#pragma once

#include "gravtree/types.h"
#include "gravtree/node.h"
#include "gravtree/manifest.h"
#include "gravtree/skiplist.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gravtree {

// ── GravTreeOptions ───────────────────────────────────────────────────────────

struct GravTreeOptions {
    std::string data_dir;

    // Root buffer soft limit (bytes).
    // Background flush is triggered when root buffer exceeds this.
    size_t   buffer_threshold  = BUFFER_THRESHOLD;

    // Hard limit = buffer_threshold * hard_limit_factor.
    // Writers block only if the background flush cannot keep up and the
    // root buffer reaches this size.  Under normal operation writers
    // never hit this ceiling.
    double   hard_limit_factor = 2.0;

    // Branching factor K: each internal node has at most fanout children.
    uint32_t fanout            = 8;

    // Maximum tree height (root level = max_height - 1).
    uint32_t max_height        = 6;

    // Bloom filter false-positive rate.
    double   bloom_fpr         = 0.01;
};

// ── GravTree ─────────────────────────────────────────────────────────────────
// A tree-structured key-value index where every node is an immutable
// SSTable file.
//
// Write path (fast):
//   insert() → append to root's in-memory sorted buffer → return
//   (no disk I/O on the critical write path)
//
// Background flush:
//   A dedicated flush thread wakes when root buffer exceeds
//   buffer_threshold, acquires the tree lock, and runs LocalFlush to
//   push data down to children.  Writers only block at the hard limit,
//   which is buffer_threshold * hard_limit_factor.
//
// Thread safety: concurrent readers and a single background writer;
//               external concurrent writers are serialised.

class GravTree {
public:
    ~GravTree();

    static std::unique_ptr<GravTree> open(GravTreeOptions opts);

    // ── Mutations ────────────────────────────────────────────────────────────
    void insert(const std::string& key, const std::string& value);
    void remove(const std::string& key);

    // ── Queries ──────────────────────────────────────────────────────────────
    std::optional<std::string> get(const std::string& key);
    std::vector<Entry>         range_scan(const std::string& start,
                                          const std::string& end);

    // Block until the background flush thread has drained all pending data.
    void flush();

    // ── Diagnostics ──────────────────────────────────────────────────────────
    struct Stats {
        size_t node_count;
        size_t tree_height;
        size_t total_flush_ops;
        size_t total_split_ops;
        size_t root_buf_bytes;       // current root buffer size
    };
    Stats stats() const;

private:
    explicit GravTree(GravTreeOptions opts);

    GravTreeOptions           opts_;
    std::unique_ptr<Manifest> manifest_;

    // ── Node cache ───────────────────────────────────────────────────────────
    std::unordered_map<NodeId, SSTNode> cache_;
    mutable std::shared_mutex           cache_mu_;

    // ── Root write buffer (skip list) ────────────────────────────────────────
    // All incoming writes land here first.  O(log n) insert — no shifting.
    // Flushed to children by the background thread via LocalFlush.
    SkipList                    write_buf_;
    std::mutex                  write_buf_mu_;  // protects write_buf_ only

    // ── Background flush thread ───────────────────────────────────────────────
    std::thread             flush_thread_;
    std::mutex              flush_cv_mu_;
    std::condition_variable flush_cv_;
    std::atomic<bool>       flush_requested_{false};
    std::atomic<bool>       flush_in_progress_{false};
    std::atomic<bool>       stop_{false};

    // ── Counters ─────────────────────────────────────────────────────────────
    std::atomic<SequenceNumber> seq_{1};
    std::atomic<size_t>         flush_ops_{0};
    std::atomic<size_t>         split_ops_{0};
    std::atomic<size_t>         root_buf_bytes_{0};

    // ── Helpers ──────────────────────────────────────────────────────────────
    SequenceNumber next_seq() noexcept {
        return seq_.fetch_add(1, std::memory_order_relaxed);
    }

    size_t hard_limit() const noexcept {
        return static_cast<size_t>(opts_.buffer_threshold * opts_.hard_limit_factor);
    }

    void    ensure_root();
    SSTNode& get_node(NodeId id);             // caller must hold cache_mu_
    std::vector<NodeId> find_path(const std::string& key); // caller holds cache_mu_

    void maybe_flush(NodeId id, const std::vector<NodeId>& path);
    void do_flush(NodeId parent_id);
    void split_leaf(NodeId leaf_id, const std::vector<NodeId>& path);
    void insert_child(NodeId parent_id, const std::string& fence_key,
                      NodeId old_child_id, NodeId left_id, NodeId right_id);
    std::vector<NodeId> path_to(NodeId target);   // DFS root→target via child_ids

    // Background flush thread entry point.
    void flush_thread_fn();
    void signal_flush();
    void wait_for_flush_complete();
};

} // namespace gravtree
