#include "gravtree/tree.h"
#include "gravtree/flush.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <stdexcept>

namespace gravtree {
namespace fs = std::filesystem;

// ── Constructor / open ────────────────────────────────────────────────────────

GravTree::GravTree(GravTreeOptions opts)
    : opts_(std::move(opts)),
      manifest_(std::make_unique<Manifest>(opts_.data_dir))
{}

GravTree::~GravTree() {
    // Stop background thread first, then flush remaining data.
    stop_.store(true, std::memory_order_release);
    flush_cv_.notify_all();
    if (flush_thread_.joinable()) flush_thread_.join();

    try {
        // Drain write buffer into root.
        std::vector<Entry> pending;
        {
            std::lock_guard<std::mutex> wlk(write_buf_mu_);
            pending = write_buf_.to_sorted_vector();
            write_buf_.clear();
        }

        std::unique_lock<std::shared_mutex> lk(cache_mu_);
        NodeId rid = manifest_->root_id();
        if (rid != INVALID_NODE_ID) {
            SSTNode& root = get_node(rid);
            for (auto& e : pending)
                root.buffer.push_back(std::move(e));
            std::stable_sort(root.buffer.begin(), root.buffer.end());
            if (!root.buffer.empty()) {
                root.flush_to_disk(opts_.data_dir);
                manifest_->register_node(rid, root.level, root.file_path);
                manifest_->save();
            }
        }
    } catch (...) {}
}

std::unique_ptr<GravTree> GravTree::open(GravTreeOptions opts) {
    fs::create_directories(opts.data_dir);
    auto tree = std::unique_ptr<GravTree>(new GravTree(std::move(opts)));
    tree->manifest_->load();
    tree->ensure_root();

    // Start the background flush thread after the tree is fully initialised.
    tree->flush_thread_ = std::thread(&GravTree::flush_thread_fn, tree.get());
    return tree;
}

// ── Background flush thread ───────────────────────────────────────────────────

void GravTree::flush_thread_fn() {
    while (true) {
        // Wait for a flush request or shutdown signal.
        {
            std::unique_lock<std::mutex> lk(flush_cv_mu_);
            flush_cv_.wait(lk, [this] {
                return flush_requested_.load(std::memory_order_acquire)
                    || stop_.load(std::memory_order_acquire);
            });
        }

        if (stop_.load(std::memory_order_acquire)) break;

        flush_in_progress_.store(true, std::memory_order_release);

        try {
            // Drain the write buffer into the root node's buffer under both locks.
            std::vector<Entry> pending;
            {
                std::lock_guard<std::mutex> wlk(write_buf_mu_);
                pending = write_buf_.to_sorted_vector();
                write_buf_.clear();
            }

            std::unique_lock<std::shared_mutex> tree_lk(cache_mu_);
            NodeId root_id = manifest_->root_id();
            SSTNode& root  = get_node(root_id);

            // Merge pending entries into root buffer.
            for (auto& e : pending)
                root.buffer.push_back(std::move(e));
            std::stable_sort(root.buffer.begin(), root.buffer.end());

            if (root.buffer_full()) {
                if (root.is_leaf() && root.num_children == 0) {
                    auto path = find_path(root.buffer.empty()
                                          ? std::string{}
                                          : root.buffer.front().key);
                    split_leaf(root_id, path);
                } else if (root.num_children > 0) {
                    do_flush(root_id);
                }
            }
            // Update the approximate buffer size counter after flush.
            root_buf_bytes_.store(write_buf_.size_bytes(),
                                  std::memory_order_release);
        } catch (...) {
            // Never let an exception kill the flush thread.
        }

        flush_requested_.store(false, std::memory_order_release);
        flush_in_progress_.store(false, std::memory_order_release);
        flush_cv_.notify_all();   // wake any blocked writers (hard-limit wait)
    }
}

void GravTree::signal_flush() {
    if (!flush_requested_.exchange(true, std::memory_order_acq_rel)) {
        flush_cv_.notify_one();
    }
}

void GravTree::wait_for_flush_complete() {
    std::unique_lock<std::mutex> lk(flush_cv_mu_);
    flush_cv_.wait(lk, [this] {
        return !flush_in_progress_.load(std::memory_order_acquire)
            || stop_.load(std::memory_order_acquire);
    });
}

// ── ensure_root ───────────────────────────────────────────────────────────────

void GravTree::ensure_root() {
    if (manifest_->root_id() != INVALID_NODE_ID) {
        std::string path = manifest_->node_path(manifest_->root_id());
        SSTNode root = SSTNode::load_header(path);
        root.load_entries();
        root_buf_bytes_.store(root.buffer_size_bytes(), std::memory_order_relaxed);
        cache_[root.id] = std::move(root);
        return;
    }
    NodeId rid = manifest_->next_node_id();
    SSTNode root = SSTNode::create(rid, 0, {}, {}, opts_.data_dir);
    manifest_->register_node(rid, 0, root.file_path);
    manifest_->set_root(rid);
    manifest_->save();
    root_buf_bytes_.store(0, std::memory_order_relaxed);
    cache_[rid] = std::move(root);
}

// ── get_node ──────────────────────────────────────────────────────────────────

SSTNode& GravTree::get_node(NodeId id) {
    auto it = cache_.find(id);
    if (it != cache_.end()) return it->second;

    std::string path = manifest_->node_path(id);
    SSTNode node = SSTNode::load_header(path);
    node.load_entries();
    cache_[id] = std::move(node);
    return cache_[id];
}

// ── find_path ─────────────────────────────────────────────────────────────────

std::vector<NodeId> GravTree::find_path(const std::string& key) {
    std::vector<NodeId> path;
    NodeId cur = manifest_->root_id();

    while (true) {
        path.push_back(cur);
        SSTNode& node = get_node(cur);
        if (node.is_leaf()) break;

        NodeId next = node.child_ids[0];
        for (uint32_t i = 0; i < node.num_children; ++i) {
            SSTNode& child = get_node(node.child_ids[i]);
            if (key >= child.key_range.min_key) next = node.child_ids[i];
            else break;
        }
        cur = next;
    }
    return path;
}

// ── insert ────────────────────────────────────────────────────────────────────

void GravTree::insert(const std::string& key, const std::string& value) {
    // Hard-limit back-pressure: if the background flush is far behind, block.
    // Under normal operation this path is never taken.
    if (write_buf_.size_bytes() >= hard_limit()) {
        wait_for_flush_complete();
    }

    {
        std::lock_guard<std::mutex> lk(write_buf_mu_);
        write_buf_.insert(Entry{key, value, next_seq(), false});
    }

    // Signal background flush when soft threshold is crossed.
    if (write_buf_.size_bytes() >= opts_.buffer_threshold) {
        signal_flush();
    }
}

// ── remove ────────────────────────────────────────────────────────────────────

void GravTree::remove(const std::string& key) {
    if (write_buf_.size_bytes() >= hard_limit()) {
        wait_for_flush_complete();
    }

    {
        std::lock_guard<std::mutex> lk(write_buf_mu_);
        write_buf_.insert(Entry{key, {}, next_seq(), true});
    }

    if (write_buf_.size_bytes() >= opts_.buffer_threshold) {
        signal_flush();
    }
}

// ── get ───────────────────────────────────────────────────────────────────────

std::optional<std::string> GravTree::get(const std::string& key) {
    // 1. Check the hot write buffer first (O(log n), no disk I/O).
    {
        std::lock_guard<std::mutex> lk(write_buf_mu_);
        if (const Entry* e = write_buf_.find_key(key)) {
            if (e->deleted) return std::nullopt;
            return e->value;
        }
    }

    // 2. Walk the on-disk node tree.
    std::unique_lock<std::shared_mutex> lk(cache_mu_);
    auto path = find_path(key);
    for (NodeId nid : path) {
        SSTNode& node = get_node(nid);
        if (!node.might_contain(key)) continue;
        auto entry = node.get(key);
        if (entry) {
            if (entry->deleted) return std::nullopt;
            return entry->value;
        }
    }
    return std::nullopt;
}

// ── range_scan ────────────────────────────────────────────────────────────────

std::vector<Entry> GravTree::range_scan(const std::string& start,
                                        const std::string& end) {
    std::vector<Entry> collected;

    // Collect from write buffer.
    {
        std::lock_guard<std::mutex> lk(write_buf_mu_);
        for (const auto& e : write_buf_)
            if (e.key >= start && e.key <= end)
                collected.push_back(e);
    }

    // Collect from on-disk nodes.
    std::unique_lock<std::shared_mutex> lk(cache_mu_);
    for (auto& [id, node] : cache_) {
        if (!node.is_loaded()) node.load_entries();
        for (auto& e : node.buffer)
            if (e.key >= start && e.key <= end)
                collected.push_back(e);
    }

    std::stable_sort(collected.begin(), collected.end());
    std::vector<Entry> result;
    for (auto& e : collected) {
        if (!result.empty() && result.back().key == e.key) continue;
        if (!e.deleted) result.push_back(e);
    }
    return result;
}

// ── flush ─────────────────────────────────────────────────────────────────────

void GravTree::flush() {
    // Ask background thread to flush, then wait for it.
    signal_flush();
    wait_for_flush_complete();

    // Drain any remaining write buffer entries into root and persist.
    std::vector<Entry> pending;
    {
        std::lock_guard<std::mutex> wlk(write_buf_mu_);
        pending = write_buf_.to_sorted_vector();
        write_buf_.clear();
    }

    std::unique_lock<std::shared_mutex> lk(cache_mu_);
    NodeId rid = manifest_->root_id();
    SSTNode& root = get_node(rid);

    for (auto& e : pending)
        root.buffer.push_back(std::move(e));
    std::stable_sort(root.buffer.begin(), root.buffer.end());

    if (!root.buffer.empty()) {
        root.flush_to_disk(opts_.data_dir);
        manifest_->register_node(rid, root.level, root.file_path);
        manifest_->save();
    }
}

// ── maybe_flush / do_flush ────────────────────────────────────────────────────

void GravTree::maybe_flush(NodeId id, const std::vector<NodeId>& path) {
    SSTNode& node = get_node(id);
    if (!node.buffer_full()) return;

    if (node.is_leaf() && node.num_children == 0) {
        split_leaf(id, path);
        return;
    }
    if (node.num_children > 0) do_flush(id);
}

void GravTree::do_flush(NodeId parent_id) {
    SSTNode& parent = get_node(parent_id);

    std::vector<SSTNode> children;
    children.reserve(parent.num_children);
    for (uint32_t i = 0; i < parent.num_children; ++i) {
        SSTNode& c = get_node(parent.child_ids[i]);
        if (!c.is_loaded()) c.load_entries();
        children.push_back(c);
    }

    FlushResult result = local_flush(parent, children, *manifest_, opts_.data_dir);
    flush_ops_.fetch_add(1, std::memory_order_relaxed);

    // Update parent child pointers.
    for (uint32_t i = 0, j = 0; i < parent.num_children; ++i) {
        NodeId old_id = parent.child_ids[i];
        for (size_t k = 0; k < result.old_child_ids.size(); ++k) {
            if (result.old_child_ids[k] == old_id) {
                parent.child_ids[i] = result.new_child_ids[j];
                break;
            }
        }
        ++j;
    }

    // Refresh cache with new children. Internal nodes rewritten by
    // local_flush get a key_range derived from their entry partition —
    // widen it to span their children before persisting again.
    for (size_t i = 0; i < children.size(); ++i) {
        if (i < result.old_child_ids.size())
            cache_.erase(result.old_child_ids[i]);
        SSTNode& c = children[i];
        if (c.num_children > 0) {
            const std::string cmin = get_node(c.child_ids[0]).key_range.min_key;
            const std::string cmax = get_node(c.child_ids[c.num_children - 1]).key_range.max_key;
            if (c.key_range.min_key.empty() || cmin < c.key_range.min_key)
                c.key_range.min_key = cmin;
            if (c.key_range.max_key.empty() || cmax > c.key_range.max_key)
                c.key_range.max_key = cmax;
            c.flush_to_disk(opts_.data_dir);
            manifest_->register_node(c.id, c.level, c.file_path);
        }
        cache_[c.id] = std::move(c);
    }

    parent.buffer.clear();
    // Internal key_range must span the children for find_path routing.
    if (parent.num_children > 0) {
        parent.key_range.min_key = get_node(parent.child_ids[0]).key_range.min_key;
        parent.key_range.max_key = get_node(parent.child_ids[parent.num_children - 1]).key_range.max_key;
    }
    parent.flush_to_disk(opts_.data_dir);
    manifest_->register_node(parent.id, parent.level, parent.file_path);
    manifest_->save();
    manifest_->gc();   // delete invalidated old child files

    // Recursively handle any children that overflowed after receiving data.
    // Snapshot the IDs first; split/flush may modify the parent's child list.
    std::vector<NodeId> to_check = result.new_child_ids;
    for (NodeId cid : to_check) {
        auto it = cache_.find(cid);
        if (it == cache_.end()) continue;   // already erased by a prior split
        SSTNode& child = it->second;
        if (!child.buffer_full()) continue;
        if (child.is_leaf() && child.num_children == 0) {
            split_leaf(cid, {parent_id, cid});
        } else if (child.num_children > 0) {
            do_flush(cid);
        }
    }
}

// ── split_leaf ────────────────────────────────────────────────────────────────

void GravTree::split_leaf(NodeId leaf_id, const std::vector<NodeId>& path) {
    SSTNode& leaf = get_node(leaf_id);

    std::stable_sort(leaf.buffer.begin(), leaf.buffer.end());
    size_t mid = leaf.buffer.size() / 2;
    std::string fence_key = leaf.buffer[mid].key;

    std::vector<Entry> left_entries(leaf.buffer.begin(),
                                    leaf.buffer.begin() + mid);
    std::vector<Entry> right_entries(leaf.buffer.begin() + mid,
                                     leaf.buffer.end());

    NodeId left_id  = manifest_->next_node_id();
    NodeId right_id = manifest_->next_node_id();

    SSTNode left_node  = SSTNode::create(left_id,  0, std::move(left_entries),  {}, opts_.data_dir);
    SSTNode right_node = SSTNode::create(right_id, 0, std::move(right_entries), {}, opts_.data_dir);

    manifest_->register_node(left_id,  0, left_node.file_path);
    manifest_->register_node(right_id, 0, right_node.file_path);
    manifest_->invalidate_node(leaf_id);
    split_ops_.fetch_add(1, std::memory_order_relaxed);

    NodeId lid = left_id, rid = right_id;
    cache_[left_id]  = std::move(left_node);
    cache_[right_id] = std::move(right_node);
    cache_.erase(leaf_id);

    if (path.size() == 1) {
        NodeId new_root_id = manifest_->next_node_id();
        SSTNode new_root = SSTNode::create(new_root_id, 1, {},
                                           {lid, rid}, opts_.data_dir);
        new_root.key_range.min_key = cache_[lid].key_range.min_key;
        new_root.key_range.max_key = cache_[rid].key_range.max_key;
        new_root.flush_to_disk(opts_.data_dir);   // persist key_range
        manifest_->register_node(new_root_id, 1, new_root.file_path);
        manifest_->set_root(new_root_id);
        cache_[new_root_id] = std::move(new_root);
    } else {
        NodeId parent_id = path[path.size() - 2];
        insert_child(parent_id, fence_key, leaf_id, lid, rid);
    }
    manifest_->save();
    manifest_->gc();   // delete the invalidated old leaf file
}

// ── path_to ───────────────────────────────────────────────────────────────────

// DFS from the root through child_ids. Caller holds cache_mu_.
std::vector<NodeId> GravTree::path_to(NodeId target) {
    std::vector<NodeId> path;
    std::function<bool(NodeId)> dfs = [&](NodeId id) -> bool {
        path.push_back(id);
        if (id == target) return true;
        SSTNode& node = get_node(id);
        for (uint32_t i = 0; i < node.num_children; ++i)
            if (dfs(node.child_ids[i])) return true;
        path.pop_back();
        return false;
    };
    dfs(manifest_->root_id());
    return path;
}

// ── insert_child ─────────────────────────────────────────────────────────────

void GravTree::insert_child(NodeId parent_id, const std::string& /*fence_key*/,
                             NodeId old_child_id, NodeId left_id, NodeId right_id) {
    SSTNode& parent = get_node(parent_id);
    if (parent.num_children < MAX_FANOUT) {
        for (uint32_t i = 0; i < parent.num_children; ++i) {
            if (parent.child_ids[i] == old_child_id) {
                // Shift everything after position i+1 right by one slot.
                for (uint32_t j = parent.num_children; j > i + 1; --j)
                    parent.child_ids[j] = parent.child_ids[j - 1];
                parent.child_ids[i]     = left_id;
                parent.child_ids[i + 1] = right_id;
                parent.num_children++;
                goto done;
            }
        }
    }
    if (parent.num_children < MAX_FANOUT) goto done;

    // Parent is full: split the internal node in two and propagate upward.
    // Copy everything we need first — the parent ref dangles after erase.
    {
        const uint32_t plevel = parent.level;
        const NodeId   pid    = parent.id;
        std::vector<NodeId> kids(parent.child_ids,
                                 parent.child_ids + parent.num_children);
        std::vector<Entry> pbuf = parent.buffer;   // usually empty

        auto it = std::find(kids.begin(), kids.end(), old_child_id);
        if (it == kids.end()) goto done;   // stale pointer — nothing to replace
        size_t pos = static_cast<size_t>(it - kids.begin());
        kids[pos] = left_id;
        kids.insert(kids.begin() + pos + 1, right_id);   // MAX_FANOUT + 1 children

        const size_t half = kids.size() / 2;
        std::vector<NodeId> lk(kids.begin(), kids.begin() + half);
        std::vector<NodeId> rk(kids.begin() + half, kids.end());

        // Partition any buffered entries between the two halves.
        const std::string right_min = get_node(rk.front()).key_range.min_key;
        std::vector<Entry> lbuf, rbuf;
        for (auto& e : pbuf)
            (e.key < right_min ? lbuf : rbuf).push_back(std::move(e));

        NodeId lid2 = manifest_->next_node_id();
        NodeId rid2 = manifest_->next_node_id();
        SSTNode ln = SSTNode::create(lid2, plevel, std::move(lbuf), lk, opts_.data_dir);
        SSTNode rn = SSTNode::create(rid2, plevel, std::move(rbuf), rk, opts_.data_dir);

        // Internal key_range spans the children's key ranges (not the buffer's).
        auto span = [&](SSTNode& n, const std::vector<NodeId>& cs) {
            n.key_range.min_key = get_node(cs.front()).key_range.min_key;
            n.key_range.max_key = get_node(cs.back()).key_range.max_key;
        };
        span(ln, lk);
        span(rn, rk);
        // flush_to_disk only rewrites key_range when the buffer is non-empty;
        // internal-node buffers are empty on all reachable paths, so this
        // persists the children-span range correctly.
        ln.flush_to_disk(opts_.data_dir);
        rn.flush_to_disk(opts_.data_dir);

        std::vector<NodeId> path = path_to(pid);   // before invalidation
        manifest_->invalidate_node(pid);
        manifest_->register_node(lid2, plevel, ln.file_path);
        manifest_->register_node(rid2, plevel, rn.file_path);
        cache_[lid2] = std::move(ln);
        cache_[rid2] = std::move(rn);
        cache_.erase(pid);

        if (path.size() == 1) {
            // Split the root: grow the tree by one level.
            NodeId new_root_id = manifest_->next_node_id();
            SSTNode new_root = SSTNode::create(new_root_id, plevel + 1, {},
                                               {lid2, rid2}, opts_.data_dir);
            new_root.key_range.min_key = cache_[lid2].key_range.min_key;
            new_root.key_range.max_key = cache_[rid2].key_range.max_key;
            new_root.flush_to_disk(opts_.data_dir);
            manifest_->register_node(new_root_id, plevel + 1,
                                     new_root.file_path);
            manifest_->set_root(new_root_id);
            cache_[new_root_id] = std::move(new_root);
        } else {
            insert_child(path[path.size() - 2], right_min, pid, lid2, rid2);
        }
        manifest_->save();
        manifest_->gc();
        return;
    }

done:
    if (parent.num_children > 0) {
        parent.key_range.min_key = get_node(parent.child_ids[0]).key_range.min_key;
        parent.key_range.max_key = get_node(parent.child_ids[parent.num_children - 1]).key_range.max_key;
    }
    parent.flush_to_disk(opts_.data_dir);
    manifest_->register_node(parent.id, parent.level, parent.file_path);
}

// ── stats ─────────────────────────────────────────────────────────────────────

GravTree::Stats GravTree::stats() const {
    std::shared_lock<std::shared_mutex> lk(cache_mu_);
    return Stats{
        cache_.size(), 1,
        flush_ops_.load(), split_ops_.load(),
        root_buf_bytes_.load()
    };
}

} // namespace gravtree
