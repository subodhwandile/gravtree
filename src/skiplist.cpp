#include "gravtree/skiplist.h"

#include <cassert>
#include <stdexcept>

namespace gravtree {

// ── Constructor / destructor ──────────────────────────────────────────────────

SkipList::SkipList() {
    head_ = new Node(kMaxHeight);   // sentinel — value unused
}

SkipList::~SkipList() {
    Node* cur = head_;
    while (cur) {
        Node* nxt = cur->next[0];
        delete cur;
        cur = nxt;
    }
}

// ── random_height ─────────────────────────────────────────────────────────────

int SkipList::random_height() noexcept {
    int h = 1;
    while (h < kMaxHeight && (rng_() % kBranching == 0)) ++h;
    return h;
}

// ── find_prev ─────────────────────────────────────────────────────────────────
// Walk from the tallest level downward.  At each level advance as far right
// as possible while next->val < val.  Store the predecessor at every level in
// prev_out (if non-null).  Returns the level-0 predecessor.

SkipList::Node* SkipList::find_prev(const Entry& val,
                                     Node** prev_out) const noexcept {
    Node* cur = head_;
    for (int lv = cur_height_ - 1; lv >= 0; --lv) {
        while (cur->next[lv] != nullptr && cur->next[lv]->val < val)
            cur = cur->next[lv];
        if (prev_out) prev_out[lv] = cur;
    }
    return cur;
}

// ── insert ────────────────────────────────────────────────────────────────────

void SkipList::insert(Entry val) {
    Node* prev[kMaxHeight];
    find_prev(val, prev);

    int h = random_height();
    if (h > cur_height_) {
        for (int i = cur_height_; i < h; ++i)
            prev[i] = head_;
        cur_height_ = h;
    }

    bytes_ += val.key.size() + val.value.size() + sizeof(SequenceNumber) + 1;
    Node* nd = new Node(std::move(val), h);
    for (int i = 0; i < h; ++i) {
        nd->next[i]   = prev[i]->next[i];
        prev[i]->next[i] = nd;
    }
    ++size_;
}

// ── find_key ─────────────────────────────────────────────────────────────────
// Because entries sort (key asc, seq_no DESC), the entry with the highest
// seq_no for a given key comes FIRST among entries with that key.
// We probe with seq_no = MAX to land just before the first real entry for key.

const Entry* SkipList::find_key(std::string_view key) const noexcept {
    // Build a probe whose sort position is just before the first real entry
    // with this key (seq_no = MAX  →  "smallest" within the key's block).
    Entry probe;
    probe.key    = std::string(key);
    probe.seq_no = std::numeric_limits<SequenceNumber>::max();
    // deleted / value don't matter for comparison

    const Node* cur = head_;
    for (int lv = cur_height_ - 1; lv >= 0; --lv) {
        while (cur->next[lv] != nullptr && cur->next[lv]->val < probe)
            cur = cur->next[lv];
    }
    const Node* candidate = cur->next[0];
    if (candidate != nullptr && candidate->val.key == key)
        return &candidate->val;
    return nullptr;
}

// ── clear ─────────────────────────────────────────────────────────────────────

void SkipList::clear() {
    Node* cur = head_->next[0];
    while (cur) {
        Node* nxt = cur->next[0];
        delete cur;
        cur = nxt;
    }
    std::fill(head_->next, head_->next + kMaxHeight, nullptr);
    cur_height_ = 1;
    size_  = 0;
    bytes_ = 0;
}

// ── to_sorted_vector ──────────────────────────────────────────────────────────

std::vector<Entry> SkipList::to_sorted_vector() const {
    std::vector<Entry> out;
    out.reserve(size_);
    for (const Node* cur = head_->next[0]; cur; cur = cur->next[0])
        out.push_back(cur->val);
    return out;
}

} // namespace gravtree
