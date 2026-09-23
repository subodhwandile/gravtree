#include "gravtree/bloom.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace gravtree {

// ── MurmurHash3 finaliser (128-bit → two 64-bit outputs) ─────────────────────

static inline uint64_t fmix64(uint64_t k) noexcept {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

void BloomFilter::base_hashes(std::string_view key,
                               uint64_t& h1, uint64_t& h2) noexcept {
    // MurmurHash3_x64_128 body
    const uint8_t* data   = reinterpret_cast<const uint8_t*>(key.data());
    const size_t   len    = key.size();
    const size_t   nblocks = len / 16;

    h1 = 0x9368f2a5c3b1d4e7ULL;
    h2 = 0x1a2b3c4d5e6f7890ULL;

    const uint64_t c1 = 0x87c37b91114253d5ULL;
    const uint64_t c2 = 0x4cf5ad432745937fULL;

    for (size_t i = 0; i < nblocks; ++i) {
        uint64_t k1, k2;
        std::memcpy(&k1, data + i * 16,     8);
        std::memcpy(&k2, data + i * 16 + 8, 8);

        k1 *= c1; k1 = (k1 << 31) | (k1 >> 33); k1 *= c2; h1 ^= k1;
        h1 = (h1 << 27) | (h1 >> 37); h1 += h2; h1 = h1 * 5 + 0x52dce729;

        k2 *= c2; k2 = (k2 << 33) | (k2 >> 31); k2 *= c1; h2 ^= k2;
        h2 = (h2 << 31) | (h2 >> 33); h2 += h1; h2 = h2 * 5 + 0x38495ab5;
    }

    const uint8_t* tail = data + nblocks * 16;
    uint64_t k1 = 0, k2 = 0;
    switch (len & 15) {
        case 15: k2 ^= static_cast<uint64_t>(tail[14]) << 48; [[fallthrough]];
        case 14: k2 ^= static_cast<uint64_t>(tail[13]) << 40; [[fallthrough]];
        case 13: k2 ^= static_cast<uint64_t>(tail[12]) << 32; [[fallthrough]];
        case 12: k2 ^= static_cast<uint64_t>(tail[11]) << 24; [[fallthrough]];
        case 11: k2 ^= static_cast<uint64_t>(tail[10]) << 16; [[fallthrough]];
        case 10: k2 ^= static_cast<uint64_t>(tail[ 9]) <<  8; [[fallthrough]];
        case  9: k2 ^= static_cast<uint64_t>(tail[ 8]);
                 k2 *= c2; k2 = (k2 << 33) | (k2 >> 31); k2 *= c1; h2 ^= k2;
                 [[fallthrough]];
        case  8: k1 ^= static_cast<uint64_t>(tail[ 7]) << 56; [[fallthrough]];
        case  7: k1 ^= static_cast<uint64_t>(tail[ 6]) << 48; [[fallthrough]];
        case  6: k1 ^= static_cast<uint64_t>(tail[ 5]) << 40; [[fallthrough]];
        case  5: k1 ^= static_cast<uint64_t>(tail[ 4]) << 32; [[fallthrough]];
        case  4: k1 ^= static_cast<uint64_t>(tail[ 3]) << 24; [[fallthrough]];
        case  3: k1 ^= static_cast<uint64_t>(tail[ 2]) << 16; [[fallthrough]];
        case  2: k1 ^= static_cast<uint64_t>(tail[ 1]) <<  8; [[fallthrough]];
        case  1: k1 ^= static_cast<uint64_t>(tail[ 0]);
                 k1 *= c1; k1 = (k1 << 31) | (k1 >> 33); k1 *= c2; h1 ^= k1;
    }

    h1 ^= len; h2 ^= len;
    h1 += h2;  h2 += h1;
    h1 = fmix64(h1);
    h2 = fmix64(h2);
    h1 += h2;  h2 += h1;
}

// ── Constructor ───────────────────────────────────────────────────────────────

BloomFilter::BloomFilter(size_t expected_entries, double fpr) {
    if (expected_entries == 0) expected_entries = 1;
    // m = -n * ln(p) / (ln 2)^2
    double m_d = -static_cast<double>(expected_entries)
                 * std::log(fpr)
                 / (std::log(2.0) * std::log(2.0));
    size_t m = static_cast<size_t>(m_d);
    m = ((m + 63) / 64) * 64;   // round up to 64-bit multiple
    bits_.assign(m / 8, 0);

    // k = (m/n) * ln2
    num_hash_funcs_ = static_cast<size_t>(
        (static_cast<double>(m) / expected_entries) * std::log(2.0) + 0.5
    );
    if (num_hash_funcs_ < 1)  num_hash_funcs_ = 1;
    if (num_hash_funcs_ > 20) num_hash_funcs_ = 20;
}

// ── Core operations ───────────────────────────────────────────────────────────

void BloomFilter::add(std::string_view key) {
    uint64_t h1, h2;
    base_hashes(key, h1, h2);
    for (size_t i = 0; i < num_hash_funcs_; ++i)
        set_bit(nth_hash(h1, h2, i));
}

bool BloomFilter::might_contain(std::string_view key) const noexcept {
    uint64_t h1, h2;
    base_hashes(key, h1, h2);
    for (size_t i = 0; i < num_hash_funcs_; ++i)
        if (!get_bit(nth_hash(h1, h2, i))) return false;
    return true;
}

// ── Serialisation ─────────────────────────────────────────────────────────────

std::vector<uint8_t> BloomFilter::serialize() const {
    std::vector<uint8_t> out;
    out.resize(4 + 8 + bits_.size());
    uint32_t nhf = static_cast<uint32_t>(num_hash_funcs_);
    uint64_t nb  = static_cast<uint64_t>(bits_.size() * 8);
    std::memcpy(out.data(),     &nhf, 4);
    std::memcpy(out.data() + 4, &nb,  8);
    std::memcpy(out.data() + 12, bits_.data(), bits_.size());
    return out;
}

BloomFilter BloomFilter::deserialize(const uint8_t* data, size_t size) {
    if (size < 12)
        throw std::runtime_error("BloomFilter::deserialize: buffer too small");
    uint32_t nhf;
    uint64_t nb;
    std::memcpy(&nhf, data,     4);
    std::memcpy(&nb,  data + 4, 8);
    size_t byte_count = static_cast<size_t>(nb / 8);
    if (size < 12 + byte_count)
        throw std::runtime_error("BloomFilter::deserialize: truncated");
    BloomFilter f;
    f.num_hash_funcs_ = nhf;
    f.bits_.assign(data + 12, data + 12 + byte_count);
    return f;
}

} // namespace gravtree
