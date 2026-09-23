#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace gravtree {

// ── Core type aliases ───────────────────────────────────────────────────────

using NodeId          = uint64_t;
using SequenceNumber  = uint64_t;

constexpr NodeId         INVALID_NODE_ID   = 0;
constexpr uint32_t       MAX_FANOUT        = 16;
constexpr size_t         BUFFER_THRESHOLD  = 4ULL * 1024 * 1024; // 4 MB
constexpr uint32_t       MAX_TREE_HEIGHT   = 8;
constexpr uint64_t       NODE_FILE_MAGIC   = 0x4752415654524545ULL; // "GRAVTREE"
constexpr uint32_t       NODE_FILE_VERSION = 1;

// ── Entry ───────────────────────────────────────────────────────────────────
// A single key-value record stored in every SSTable node.

struct Entry {
    std::string    key;
    std::string    value;
    SequenceNumber seq_no  {0};
    bool           deleted {false};   // tombstone flag

    // Sort by key ascending, then by seq_no descending (newest first).
    bool operator<(const Entry& o) const noexcept {
        if (key != o.key) return key < o.key;
        return seq_no > o.seq_no;
    }
};

// ── KeyRange ────────────────────────────────────────────────────────────────

struct KeyRange {
    std::string min_key;
    std::string max_key;

    bool contains(std::string_view k) const noexcept {
        return k >= min_key && (max_key.empty() || k <= max_key);
    }

    bool empty() const noexcept {
        return min_key.empty() && max_key.empty();
    }
};

} // namespace gravtree
