#pragma once

#include "gravtree/node.h"
#include "gravtree/manifest.h"

#include <vector>

namespace gravtree {

// ── FlushResult ───────────────────────────────────────────────────────────────
// Returned by local_flush(); the caller must update the parent's child
// pointers and save the manifest.

struct FlushResult {
    std::vector<NodeId> new_child_ids;  // replacement children (one per old child)
    std::vector<NodeId> old_child_ids;  // children that were rewritten (to invalidate)
};

// ── local_flush ───────────────────────────────────────────────────────────────
// THE core operation of GravTree.
//
// Takes the parent's buffered entries and distributes them to the
// appropriate children based on key ranges, then writes each
// affected child as a new immutable SSTable file.
//
// Invariants:
//   - parent.buffer must be sorted before calling.
//   - children.size() == parent.num_children.
//   - Each child's key range is non-overlapping and covers its subtree.
//
// After the call:
//   - New SSTable files exist for every child that received data.
//   - Old child files are invalidated in the manifest.
//   - The parent's buffer is NOT cleared here; the tree clears it after
//     updating its own file with empty buffer + new child pointers.

FlushResult local_flush(SSTNode&                    parent,
                        std::vector<SSTNode>&       children,
                        Manifest&                   manifest,
                        const std::string&          data_dir);

// ── Helpers (also used by tests) ─────────────────────────────────────────────

// Partition a sorted entry list into K buckets by child key ranges.
// entries[i] goes to bucket j if children[j].key_range.contains(entries[i].key).
// Entries that fall before all children go to bucket 0;
// entries that fall after all children go to bucket K-1.
std::vector<std::vector<Entry>> partition_entries(
    const std::vector<Entry>&    entries,
    const std::vector<SSTNode>&  children);

// Merge two sorted entry vectors, keeping the entry with the highest
// seq_no when the same key appears in both.
std::vector<Entry> merge_sorted_entries(const std::vector<Entry>& a,
                                        const std::vector<Entry>& b);

} // namespace gravtree
