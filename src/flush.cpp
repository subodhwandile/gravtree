#include "gravtree/flush.h"

#include <algorithm>
#include <cassert>

namespace gravtree {

// ── partition_entries ─────────────────────────────────────────────────────────
//
// Distributes a sorted entry list across K children using their key ranges.
// Entries whose key falls before the first child's range go to bucket 0;
// entries that fall after the last child's range go to bucket K-1.

std::vector<std::vector<Entry>> partition_entries(
    const std::vector<Entry>&   entries,
    const std::vector<SSTNode>& children)
{
    const size_t K = children.size();
    std::vector<std::vector<Entry>> buckets(K);

    for (const auto& e : entries) {
        // Find the rightmost child whose min_key <= e.key.
        // Children are ordered by key range; we do a linear scan here
        // (K ≤ MAX_FANOUT = 16, so constant cost).
        int target = 0;
        for (int i = static_cast<int>(K) - 1; i >= 0; --i) {
            if (e.key >= children[i].key_range.min_key) {
                target = i;
                break;
            }
        }
        buckets[target].push_back(e);
    }
    return buckets;
}

// ── merge_sorted_entries ──────────────────────────────────────────────────────
//
// Standard two-way sorted merge.  When both inputs carry the same key,
// the entry with the higher seq_no wins.

std::vector<Entry> merge_sorted_entries(const std::vector<Entry>& a,
                                        const std::vector<Entry>& b)
{
    std::vector<Entry> out;
    out.reserve(a.size() + b.size());

    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i].key < b[j].key) {
            out.push_back(a[i++]);
        } else if (a[i].key > b[j].key) {
            out.push_back(b[j++]);
        } else {
            // Same key — keep highest seq_no, skip the other.
            if (a[i].seq_no >= b[j].seq_no) {
                out.push_back(a[i++]);
                ++j;
            } else {
                out.push_back(b[j++]);
                ++i;
            }
        }
    }
    while (i < a.size()) out.push_back(a[i++]);
    while (j < b.size()) out.push_back(b[j++]);
    return out;
}

// ── local_flush ───────────────────────────────────────────────────────────────

FlushResult local_flush(SSTNode&              parent,
                        std::vector<SSTNode>& children,
                        Manifest&             manifest,
                        const std::string&    data_dir)
{
    assert(!children.empty());

    // 1. Sort parent buffer before partitioning.
    std::stable_sort(parent.buffer.begin(), parent.buffer.end());

    // 2. Partition parent's buffer entries across children by key range.
    auto partitions = partition_entries(parent.buffer, children);

    FlushResult result;

    // 3. For each child, merge the partition into child's existing entries
    //    and write a new immutable SSTable file.
    for (size_t i = 0; i < children.size(); ++i) {
        auto& child     = children[i];
        auto& partition = partitions[i];

        if (partition.empty()) {
            // Nothing to push to this child — reuse the same node.
            result.new_child_ids.push_back(child.id);
            continue;
        }

        // Ensure child entries are loaded for merging.
        if (!child.is_loaded()) child.load_entries();

        // Merge parent partition with child's existing data.
        auto merged = merge_sorted_entries(partition, child.buffer);

        // Build new child node (new id → new immutable file).
        NodeId new_id = manifest.next_node_id();

        std::vector<NodeId> grandchildren(
            child.child_ids, child.child_ids + child.num_children);

        SSTNode new_child = SSTNode::create(
            new_id, child.level, std::move(merged), grandchildren, data_dir);

        manifest.register_node(new_id, child.level, new_child.file_path);
        manifest.invalidate_node(child.id);

        result.new_child_ids.push_back(new_id);
        result.old_child_ids.push_back(child.id);

        // Replace child in the caller's vector so the tree can update
        // its cache without a second lookup.
        child = std::move(new_child);
    }

    return result;
}

} // namespace gravtree
