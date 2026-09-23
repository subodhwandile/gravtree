#pragma once

#include "gravtree/types.h"
#include "gravtree/bloom.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gravtree {

// ── SparseIndexEntry ─────────────────────────────────────────────────────────
// Maps every SPARSE_STEP-th key inside a node's entry list to its byte
// offset in the file, enabling O(log n) seeks during point lookups.

struct SparseIndexEntry {
    std::string key;
    uint64_t    file_offset{0};
};

constexpr uint32_t SPARSE_STEP = 128; // one index entry per 128 data entries

// ── SSTNode ──────────────────────────────────────────────────────────────────
// In-memory representation of one GravTree node.
//
// Every node is backed by an immutable on-disk file produced by
// flush_to_disk().  The in-memory buffer accepts new writes; when the
// buffer exceeds BUFFER_THRESHOLD the tree calls LocalFlush to push
// buffered data down to children and write a fresh file.
//
// On-disk format (little-endian, sequential):
//   [8]   Magic  = NODE_FILE_MAGIC
//   [4]   Version
//   [8]   node_id
//   [4]   level
//   [4]   num_children
//   [128] child_ids[MAX_FANOUT]
//   [4]   num_entries
//   str   min_key  (4-byte len prefix + bytes)
//   str   max_key  (4-byte len prefix + bytes)
//   [4]   bloom_data_len
//   [?]   bloom_data
//   ──── entries (sorted by key asc, seq_no desc) ────
//   per entry:
//     str  key   (4-byte len prefix + bytes)
//     str  value (4-byte len prefix + bytes)
//     [8]  seq_no
//     [1]  deleted
//   ──── sparse index ────
//   [4]   sparse_count
//   per sparse entry:
//     str  key (4-byte len prefix + bytes)
//     [8]  file_offset
//   [8]   CRC-64 of everything above

class SSTNode {
public:
    // ── Identity & topology ──────────────────────────────────────────────────
    NodeId   id           {INVALID_NODE_ID};
    uint32_t level        {0};
    uint32_t num_children {0};
    NodeId   child_ids[MAX_FANOUT]{};
    KeyRange key_range;

    // ── Content ──────────────────────────────────────────────────────────────
    std::vector<Entry>            buffer;       // in-memory, may be unsorted
    std::vector<SparseIndexEntry> sparse_index;
    std::unique_ptr<BloomFilter>  bloom;
    std::string                   file_path;

    // ── Constructors ─────────────────────────────────────────────────────────
    SSTNode() = default;
    SSTNode(NodeId id, uint32_t level);

    // Copy constructor clones the bloom filter.
    SSTNode(const SSTNode& other);
    SSTNode& operator=(const SSTNode& other);

    SSTNode(SSTNode&&)            = default;
    SSTNode& operator=(SSTNode&&) = default;

    // Factory: create a brand-new node, write it to dir, return populated object.
    static SSTNode create(NodeId                    id,
                          uint32_t                  level,
                          std::vector<Entry>        entries,
                          const std::vector<NodeId>& children,
                          const std::string&        dir);

    // Load header + bloom filter from an existing file (entries loaded lazily).
    static SSTNode load_header(const std::string& path);

    // Load all entries from file into buffer (idempotent).
    void load_entries();

    // Sort the in-memory buffer and write a new immutable SSTable file.
    // Returns the path of the new file.
    std::string flush_to_disk(const std::string& dir);

    // ── Query ────────────────────────────────────────────────────────────────

    // Point lookup in the in-memory buffer (buffer must be sorted).
    std::optional<Entry> get(std::string_view key) const;

    bool might_contain(std::string_view key) const noexcept {
        // In-memory buffer may contain entries added after the bloom was built.
        // Only use bloom as a guard when the buffer is empty (disk-resident data).
        if (!buffer.empty()) return true;
        return !bloom || bloom->might_contain(key);
    }

    // ── State predicates ─────────────────────────────────────────────────────
    bool is_leaf()        const noexcept { return level == 0; }
    bool is_loaded()      const noexcept { return entries_loaded_; }
    bool buffer_full()    const noexcept { return buffer_size_bytes() >= BUFFER_THRESHOLD; }

    size_t buffer_size_bytes() const noexcept;

private:
    bool entries_loaded_{false};

    void sort_buffer();
    void build_bloom();
    void build_sparse_index();
    std::string write_file(const std::string& dir) const;
    static SSTNode read_file(const std::string& path, bool load_entries);
};

} // namespace gravtree
