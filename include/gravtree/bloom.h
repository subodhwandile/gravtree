#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace gravtree {

// ── BloomFilter ─────────────────────────────────────────────────────────────
// Classic Bloom filter using k independent hash functions derived from
// two base MurmurHash3 hashes (Kirsch–Mitzenmacher technique).
//
// Guarantees:
//   - Zero false negatives.
//   - False-positive rate ≤ fpr at construction capacity.
//
// Serialisation format (little-endian):
//   [4]  num_hash_funcs
//   [8]  bit_count
//   [?]  bit array (bit_count/8 bytes)

class BloomFilter {
public:
    // Create an empty filter sized for expected_entries at the given FPR.
    BloomFilter(size_t expected_entries, double fpr = 0.01);

    // Restore from serialised bytes produced by serialize().
    static BloomFilter deserialize(const uint8_t* data, size_t size);

    void add(std::string_view key);
    bool might_contain(std::string_view key) const noexcept;

    std::vector<uint8_t> serialize() const;

    size_t num_hash_funcs() const noexcept { return num_hash_funcs_; }
    size_t bit_count()      const noexcept { return bits_.size() * 8; }

private:
    BloomFilter() = default;

    std::vector<uint8_t> bits_;
    size_t               num_hash_funcs_{0};

    // Returns two independent 64-bit hashes via MurmurHash3-finalize.
    static void base_hashes(std::string_view key,
                             uint64_t& h1, uint64_t& h2) noexcept;

    size_t nth_hash(uint64_t h1, uint64_t h2, size_t i) const noexcept {
        return static_cast<size_t>((h1 + i * h2) % bit_count());
    }

    void set_bit(size_t pos) noexcept {
        bits_[pos >> 3] |= static_cast<uint8_t>(1u << (pos & 7));
    }
    bool get_bit(size_t pos) const noexcept {
        return (bits_[pos >> 3] >> (pos & 7)) & 1u;
    }
};

} // namespace gravtree
