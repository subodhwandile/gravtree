#include "gravtree/manifest.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace gravtree {
namespace fs = std::filesystem;

Manifest::Manifest(const std::string& dir) : dir_(dir) {
    fs::create_directories(dir_);
}

std::string Manifest::manifest_path() const { return dir_ + "/manifest";     }
std::string Manifest::tmp_path()      const { return dir_ + "/manifest.tmp"; }

// ── load ─────────────────────────────────────────────────────────────────────

void Manifest::load() {
    std::lock_guard<std::mutex> lk(mu_);
    std::ifstream ifs(manifest_path());
    if (!ifs) return; // first open — no manifest yet

    std::string line;
    NodeId max_id = 0;

    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string token;
        ss >> token;

        if (token == "ROOT") {
            ss >> root_id_;
        } else if (token == "NEXT_ID") {
            NodeId nid;
            ss >> nid;
            next_id_.store(nid);
        } else if (token == "NODE") {
            NodeMeta m;
            int valid_int;
            ss >> m.id >> m.level >> valid_int >> m.file_path;
            m.valid = (valid_int != 0);
            nodes_[m.id] = m;
            if (m.id > max_id) max_id = m.id;
        }
    }

    // Make sure next_id_ is beyond any stored id.
    NodeId cur = next_id_.load();
    if (max_id + 1 > cur) next_id_.store(max_id + 1);
}

// ── save ─────────────────────────────────────────────────────────────────────

void Manifest::save() const {
    std::lock_guard<std::mutex> lk(mu_);
    {
        std::ofstream ofs(tmp_path(), std::ios::trunc);
        if (!ofs) throw std::runtime_error("Cannot write manifest: " + tmp_path());
        ofs << "VERSION 1\n";
        ofs << "ROOT "    << root_id_             << "\n";
        ofs << "NEXT_ID " << next_id_.load()      << "\n";
        for (auto& [id, m] : nodes_)
            ofs << "NODE " << m.id << " " << m.level << " "
                << (m.valid ? 1 : 0) << " " << m.file_path << "\n";
    }
    fs::rename(tmp_path(), manifest_path());
}

// ── mutations ─────────────────────────────────────────────────────────────────

void Manifest::register_node(NodeId id, uint32_t level,
                              const std::string& file_path) {
    std::lock_guard<std::mutex> lk(mu_);
    nodes_[id] = NodeMeta{id, level, file_path, true};
}

void Manifest::invalidate_node(NodeId id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = nodes_.find(id);
    if (it != nodes_.end()) it->second.valid = false;
}

void Manifest::set_root(NodeId id) {
    std::lock_guard<std::mutex> lk(mu_);
    root_id_ = id;
}

// ── queries ───────────────────────────────────────────────────────────────────

std::string Manifest::node_path(NodeId id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = nodes_.find(id);
    if (it == nodes_.end() || !it->second.valid)
        throw std::runtime_error("node_path: unknown or invalid node " +
                                 std::to_string(id));
    return it->second.file_path;
}

std::vector<NodeMeta> Manifest::nodes_at_level(uint32_t level) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<NodeMeta> result;
    for (auto& [id, m] : nodes_)
        if (m.valid && m.level == level) result.push_back(m);
    return result;
}

// ── gc ────────────────────────────────────────────────────────────────────────

void Manifest::gc() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = nodes_.begin(); it != nodes_.end(); ) {
        if (!it->second.valid) {
            std::error_code ec;
            fs::remove(it->second.file_path, ec); // ignore errors (already gone)
            it = nodes_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace gravtree
