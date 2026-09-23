#pragma once

// ── SkipList ──────────────────────────────────────────────────────────────────
// A probabilistic sorted container optimised for the GravTree write buffer.
//
// Complexity (average):
//   insert      O(log n)     — no memory shifting, unlike std::vector insert
//   find        O(log n)
//   iteration   O(n)
//
// Design:
//   Stores Entry values sorted by Entry::operator< (key asc, seq_no desc).
//   The find_key() method looks up by raw key string, returning the entry
//   with the highest seq_no (the newest version) in O(log n).
//
// This replaces the std::vector<Entry> root write buffer whose sorted-insert
// was O(n) due to element shifting.

#include "gravtree/types.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <random>
#include <string_view>
#include <vector>

namespace gravtree {

class SkipList {
public:
    static constexpr int    kMaxHeight  = 12;
    static constexpr int    kBranching  = 4;  // P = 1/4

    // ── Node ─────────────────────────────────────────────────────────────────
    struct Node {
        Entry val;
        int   height;
        Node* next[kMaxHeight];

        explicit Node(Entry v, int h)
            : val(std::move(v)), height(h) {
            std::fill(next, next + kMaxHeight, nullptr);
        }
        // Sentinel constructor (head node — value is unused).
        explicit Node(int h) : height(h) {
            std::fill(next, next + kMaxHeight, nullptr);
        }
    };

    // ── Construction / destruction ────────────────────────────────────────────
    SkipList();
    ~SkipList();

    SkipList(const SkipList&)            = delete;
    SkipList& operator=(const SkipList&) = delete;
    SkipList(SkipList&&)                 = default;
    SkipList& operator=(SkipList&&)      = default;

    // ── Mutations ─────────────────────────────────────────────────────────────
    void insert(Entry val);
    void clear();

    // ── Queries ───────────────────────────────────────────────────────────────

    // Find the most-recent (highest seq_no) entry for key.
    // Returns nullptr if no entry with that key exists.
    const Entry* find_key(std::string_view key) const noexcept;

    size_t size()       const noexcept { return size_; }
    bool   empty()      const noexcept { return size_ == 0; }
    size_t size_bytes() const noexcept { return bytes_; }

    // ── Iteration (in Entry sort order) ──────────────────────────────────────
    class const_iterator {
        const Node* cur_;
    public:
        explicit const_iterator(const Node* n) noexcept : cur_(n) {}
        const Entry& operator*()  const noexcept { return cur_->val; }
        const Entry* operator->() const noexcept { return &cur_->val; }
        const_iterator& operator++() noexcept {
            cur_ = cur_->next[0]; return *this;
        }
        bool operator==(const const_iterator& o) const noexcept {
            return cur_ == o.cur_;
        }
        bool operator!=(const const_iterator& o) const noexcept {
            return cur_ != o.cur_;
        }
        using iterator_category = std::forward_iterator_tag;
        using value_type        = Entry;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const Entry*;
        using reference         = const Entry&;
    };

    const_iterator begin() const noexcept { return const_iterator(head_->next[0]); }
    const_iterator end()   const noexcept { return const_iterator(nullptr); }

    // Materialise the contents as a sorted vector (for flush / serialisation).
    std::vector<Entry> to_sorted_vector() const;

private:
    Node*   head_;
    int     cur_height_{1};
    size_t  size_{0};
    size_t  bytes_{0};
    std::mt19937 rng_{0xdeadbeef};

    int  random_height() noexcept;

    // Walk the list to find insertion predecessors at every level.
    // Returns the last node whose value < val at level 0.
    Node* find_prev(const Entry& val, Node** prev_out) const noexcept;
};

} // namespace gravtree
