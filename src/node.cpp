#include "gravtree/node.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace gravtree {
namespace fs = std::filesystem;

// ── BinaryWriter / BinaryReader helpers ──────────────────────────────────────

namespace {

struct BinaryWriter {
    std::vector<uint8_t>& buf;

    void u8 (uint8_t  v) { buf.push_back(v); }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        buf.insert(buf.end(), s.begin(), s.end());
    }
    void raw(const void* p, size_t n) {
        auto b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    }
};

struct BinaryReader {
    const uint8_t* data;
    size_t         size;
    size_t         pos{0};

    void check(size_t n) const {
        if (pos + n > size)
            throw std::runtime_error("BinaryReader: unexpected end of data");
    }
    uint8_t  u8()  { check(1); return data[pos++]; }
    uint32_t u32() { uint32_t v; raw(&v, 4); return v; }
    uint64_t u64() { uint64_t v; raw(&v, 8); return v; }
    std::string str() {
        uint32_t len = u32();
        check(len);
        std::string s(reinterpret_cast<const char*>(data + pos), len);
        pos += len;
        return s;
    }
    void raw(void* dst, size_t n) {
        check(n);
        std::memcpy(dst, data + pos, n);
        pos += n;
    }
};

// Very simple CRC-64 (ECMA-182 poly) for data integrity.
static uint64_t crc64(const uint8_t* data, size_t len) noexcept {
    static const uint64_t POLY = 0xad93d23594c935a9ULL;
    uint64_t crc = 0xFFFFFFFFFFFFFFFFULL;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint64_t>(data[i]) << 56;
        for (int b = 0; b < 8; ++b)
            crc = (crc & (1ULL << 63)) ? (crc << 1) ^ POLY : crc << 1;
    }
    return crc ^ 0xFFFFFFFFFFFFFFFFULL;
}

} // anonymous namespace

// ── SSTNode constructors ──────────────────────────────────────────────────────

SSTNode::SSTNode(NodeId id, uint32_t level) : id(id), level(level) {}

SSTNode::SSTNode(const SSTNode& o)
    : id(o.id), level(o.level), num_children(o.num_children),
      key_range(o.key_range), buffer(o.buffer),
      sparse_index(o.sparse_index), file_path(o.file_path),
      entries_loaded_(o.entries_loaded_) {
    std::memcpy(child_ids, o.child_ids, sizeof(child_ids));
    if (o.bloom) {
        auto bytes = o.bloom->serialize();
        bloom = std::make_unique<BloomFilter>(
            BloomFilter::deserialize(bytes.data(), bytes.size()));
    }
}

SSTNode& SSTNode::operator=(const SSTNode& o) {
    if (this != &o) { SSTNode tmp(o); *this = std::move(tmp); }
    return *this;
}

// ── create ────────────────────────────────────────────────────────────────────

SSTNode SSTNode::create(NodeId                     id,
                        uint32_t                   level,
                        std::vector<Entry>         entries,
                        const std::vector<NodeId>& children,
                        const std::string&         dir) {
    SSTNode node(id, level);
    node.buffer = std::move(entries);
    node.sort_buffer();

    node.num_children = static_cast<uint32_t>(
        std::min(children.size(), static_cast<size_t>(MAX_FANOUT)));
    for (uint32_t i = 0; i < node.num_children; ++i)
        node.child_ids[i] = children[i];

    if (!node.buffer.empty()) {
        node.key_range.min_key = node.buffer.front().key;
        node.key_range.max_key = node.buffer.back().key;
    }

    node.build_bloom();
    node.build_sparse_index();
    node.file_path    = node.write_file(dir);
    node.entries_loaded_ = true;
    return node;
}

// ── load_header ───────────────────────────────────────────────────────────────

SSTNode SSTNode::load_header(const std::string& path) {
    return read_file(path, /*load_entries=*/false);
}

// ── load_entries ──────────────────────────────────────────────────────────────

void SSTNode::load_entries() {
    if (entries_loaded_) return;
    SSTNode full = read_file(file_path, /*load_entries=*/true);
    buffer         = std::move(full.buffer);
    sparse_index   = std::move(full.sparse_index);
    entries_loaded_ = true;
}

// ── flush_to_disk ─────────────────────────────────────────────────────────────

std::string SSTNode::flush_to_disk(const std::string& dir) {
    sort_buffer();
    if (!buffer.empty()) {
        // Expand (never shrink) the persisted range: internal nodes carry a
        // key_range spanning their children that must survive buffer flushes.
        if (key_range.min_key.empty() || buffer.front().key < key_range.min_key)
            key_range.min_key = buffer.front().key;
        if (key_range.max_key.empty() || buffer.back().key > key_range.max_key)
            key_range.max_key = buffer.back().key;
    }
    build_bloom();
    build_sparse_index();
    file_path = write_file(dir);
    entries_loaded_ = true;
    return file_path;
}

// ── get ───────────────────────────────────────────────────────────────────────

std::optional<Entry> SSTNode::get(std::string_view key) const {
    if (buffer.empty()) return std::nullopt;
    // Binary search for the first entry with matching key.
    auto it = std::lower_bound(buffer.begin(), buffer.end(),
        Entry{std::string(key), {}, std::numeric_limits<SequenceNumber>::max()});
    if (it != buffer.end() && it->key == key) return *it;
    return std::nullopt;
}

// ── buffer_size_bytes ─────────────────────────────────────────────────────────

size_t SSTNode::buffer_size_bytes() const noexcept {
    size_t total = 0;
    for (auto& e : buffer)
        total += e.key.size() + e.value.size() + sizeof(SequenceNumber) + 1;
    return total;
}

// ── sort_buffer ───────────────────────────────────────────────────────────────

void SSTNode::sort_buffer() {
    std::stable_sort(buffer.begin(), buffer.end());
    // Deduplicate: keep only the highest seq_no per key.
    if (buffer.size() <= 1) return;
    std::vector<Entry> deduped;
    deduped.reserve(buffer.size());
    for (auto& e : buffer) {
        if (!deduped.empty() && deduped.back().key == e.key) continue; // lower seq_no
        deduped.push_back(std::move(e));
    }
    buffer = std::move(deduped);
}

// ── build_bloom ───────────────────────────────────────────────────────────────

void SSTNode::build_bloom() {
    bloom = std::make_unique<BloomFilter>(
        std::max(buffer.size(), size_t{1}), 0.01);
    for (auto& e : buffer)
        bloom->add(e.key);
}

// ── build_sparse_index ────────────────────────────────────────────────────────

void SSTNode::build_sparse_index() {
    sparse_index.clear();
    // File offsets are not known yet at this stage;
    // they will be filled during write_file().
    for (size_t i = 0; i < buffer.size(); i += SPARSE_STEP)
        sparse_index.push_back({buffer[i].key, 0});
}

// ── write_file ────────────────────────────────────────────────────────────────

std::string SSTNode::write_file(const std::string& dir) const {
    fs::create_directories(dir);
    std::string path = dir + "/node_" + std::to_string(id) + ".gst";

    std::vector<uint8_t> raw_buf;
    raw_buf.reserve(1024 * 1024);
    BinaryWriter w{raw_buf};

    // ── Header ────────────────────────────────────────────────────────────────
    w.u64(NODE_FILE_MAGIC);
    w.u32(NODE_FILE_VERSION);
    w.u64(id);
    w.u32(level);
    w.u32(num_children);
    for (uint32_t i = 0; i < MAX_FANOUT; ++i)
        w.u64(child_ids[i]);
    w.u32(static_cast<uint32_t>(buffer.size()));
    w.str(key_range.min_key);
    w.str(key_range.max_key);

    // ── Bloom filter ──────────────────────────────────────────────────────────
    if (bloom) {
        auto bd = bloom->serialize();
        w.u32(static_cast<uint32_t>(bd.size()));
        w.raw(bd.data(), bd.size());
    } else {
        w.u32(0);
    }

    // ── Entries + build sparse index offsets in one pass ─────────────────────
    std::vector<std::pair<size_t /*entry_idx*/, uint64_t /*offset*/>> sparse_offsets;
    for (size_t i = 0; i < buffer.size(); ++i) {
        if (i % SPARSE_STEP == 0)
            sparse_offsets.push_back({i, static_cast<uint64_t>(raw_buf.size())});
        const Entry& e = buffer[i];
        w.str(e.key);
        w.str(e.value);
        w.u64(e.seq_no);
        w.u8 (e.deleted ? 1 : 0);
    }

    // ── Sparse index ─────────────────────────────────────────────────────────
    w.u32(static_cast<uint32_t>(sparse_offsets.size()));
    for (auto& [idx, off] : sparse_offsets) {
        w.str(buffer[idx].key);
        w.u64(off);
    }

    // ── Checksum ─────────────────────────────────────────────────────────────
    uint64_t checksum = crc64(raw_buf.data(), raw_buf.size());
    w.u64(checksum);

    // Write atomically: tmp → rename
    std::string tmp = path + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) throw std::runtime_error("Cannot open " + tmp + " for writing");
        ofs.write(reinterpret_cast<const char*>(raw_buf.data()),
                  static_cast<std::streamsize>(raw_buf.size()));
    }
    fs::rename(tmp, path);
    return path;
}

// ── read_file ─────────────────────────────────────────────────────────────────

SSTNode SSTNode::read_file(const std::string& path, bool load_entries) {
    // Slurp entire file.
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) throw std::runtime_error("Cannot open node file: " + path);
    auto file_size = static_cast<size_t>(ifs.tellg());
    ifs.seekg(0);
    std::vector<uint8_t> raw(file_size);
    ifs.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(file_size));

    if (file_size < 8) throw std::runtime_error("Node file too small: " + path);

    // Verify checksum (last 8 bytes).
    uint64_t stored_crc;
    std::memcpy(&stored_crc, raw.data() + file_size - 8, 8);
    uint64_t computed_crc = crc64(raw.data(), file_size - 8);
    if (stored_crc != computed_crc)
        throw std::runtime_error("Checksum mismatch in node file: " + path);

    BinaryReader r{raw.data(), file_size - 8 /* exclude checksum */};

    // ── Header ────────────────────────────────────────────────────────────────
    uint64_t magic = r.u64();
    if (magic != NODE_FILE_MAGIC)
        throw std::runtime_error("Bad magic in node file: " + path);
    uint32_t version = r.u32();
    if (version != NODE_FILE_VERSION)
        throw std::runtime_error("Unsupported node file version: " + path);

    SSTNode node;
    node.file_path    = path;
    node.id           = r.u64();
    node.level        = r.u32();
    node.num_children = r.u32();
    for (uint32_t i = 0; i < MAX_FANOUT; ++i)
        node.child_ids[i] = r.u64();
    uint32_t num_entries = r.u32();
    node.key_range.min_key = r.str();
    node.key_range.max_key = r.str();

    // ── Bloom filter ──────────────────────────────────────────────────────────
    uint32_t bloom_len = r.u32();
    if (bloom_len > 0) {
        node.bloom = std::make_unique<BloomFilter>(
            BloomFilter::deserialize(r.data + r.pos, bloom_len));
        r.pos += bloom_len;
    }

    if (!load_entries) return node;

    // ── Entries ───────────────────────────────────────────────────────────────
    node.buffer.reserve(num_entries);
    for (uint32_t i = 0; i < num_entries; ++i) {
        Entry e;
        e.key     = r.str();
        e.value   = r.str();
        e.seq_no  = r.u64();
        e.deleted = (r.u8() != 0);
        node.buffer.push_back(std::move(e));
    }
    node.entries_loaded_ = true;

    // ── Sparse index ─────────────────────────────────────────────────────────
    uint32_t si_count = r.u32();
    node.sparse_index.reserve(si_count);
    for (uint32_t i = 0; i < si_count; ++i) {
        SparseIndexEntry se;
        se.key         = r.str();
        se.file_offset = r.u64();
        node.sparse_index.push_back(std::move(se));
    }

    return node;
}

} // namespace gravtree
