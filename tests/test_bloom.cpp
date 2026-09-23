#include <gtest/gtest.h>
#include "gravtree/bloom.h"

using namespace gravtree;

TEST(BloomFilter, NoFalseNegatives) {
    BloomFilter bf(1000);
    std::vector<std::string> keys;
    for (int i = 0; i < 1000; ++i) {
        keys.push_back("key_" + std::to_string(i));
        bf.add(keys.back());
    }
    for (auto& k : keys)
        EXPECT_TRUE(bf.might_contain(k)) << "False negative for: " << k;
}

TEST(BloomFilter, FalsePositiveRateWithinBounds) {
    const size_t N   = 10'000;
    const double FPR = 0.01;
    BloomFilter bf(N, FPR);

    for (size_t i = 0; i < N; ++i)
        bf.add("inserted_" + std::to_string(i));

    size_t fp = 0;
    const size_t CHECKS = 100'000;
    for (size_t i = 0; i < CHECKS; ++i)
        if (bf.might_contain("absent_" + std::to_string(i))) ++fp;

    double actual_fpr = static_cast<double>(fp) / CHECKS;
    EXPECT_LT(actual_fpr, FPR * 3) // allow 3× tolerance
        << "FPR too high: " << actual_fpr;
}

TEST(BloomFilter, SerialiseRoundTrip) {
    BloomFilter bf(500);
    for (int i = 0; i < 500; ++i)
        bf.add("k" + std::to_string(i));

    auto bytes = bf.serialize();
    BloomFilter bf2 = BloomFilter::deserialize(bytes.data(), bytes.size());

    for (int i = 0; i < 500; ++i)
        EXPECT_TRUE(bf2.might_contain("k" + std::to_string(i)));
}

TEST(BloomFilter, EmptyFilterContainsNothing) {
    BloomFilter bf(100);
    EXPECT_FALSE(bf.might_contain("anything"));
}
