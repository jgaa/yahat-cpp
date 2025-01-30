#include <gtest/gtest.h>
#include "yahat/Metrics.h"  // Ensure this includes the Histogram class

using namespace yahat;

class HistogramTest : public ::testing::Test {
protected:
    void SetUp() override {
        yahat::Metrics::labels_t labels = {{"api", "test"}};
        std::vector<double> bucket_bounds = {0.1, 0.5, 1.0, 5.0};
        histogram = metrics.AddHistogram("request_duration", "Request duration distribution", "sec", labels, bucket_bounds);
    }

    Metrics metrics;
    Metrics::Histogram<double>* histogram{};
};

TEST_F(HistogramTest, InitialState) {
    EXPECT_EQ(histogram->getCount(), 0);
    EXPECT_DOUBLE_EQ(histogram->getSum(), 0.0);
}

TEST_F(HistogramTest, ObserveSingleValue) {
    histogram->observe(0.3);
    EXPECT_EQ(histogram->getCount(), 1);
    EXPECT_DOUBLE_EQ(histogram->getSum(), 0.3);
}

TEST_F(HistogramTest, ObserveMultipleValues) {
    histogram->observe(0.2);
    histogram->observe(0.6);
    histogram->observe(1.2);

    EXPECT_EQ(histogram->getCount(), 3);
    EXPECT_DOUBLE_EQ(histogram->getSum(), 2.0);
}

TEST_F(HistogramTest, BucketCount) {
    histogram->observe(0.05); // Should fall into the first bucket
    histogram->observe(0.3);  // Second bucket
    histogram->observe(0.7);  // Third bucket
    histogram->observe(2.0);  // Fourth bucket
    histogram->observe(10.0); // Overflow bucket (+Inf)

    auto buckets = histogram->getBucketCounts();
    ASSERT_EQ(buckets.size(), 5);
    EXPECT_EQ(buckets[0], 1); // 0.05 in first bucket
    EXPECT_EQ(buckets[1], 1); // 0.3 in second bucket
    EXPECT_EQ(buckets[2], 1); // 0.7 in third bucket
    EXPECT_EQ(buckets[3], 1); // 2.0 in fourth bucket
    EXPECT_EQ(buckets[4], 1); // 10.0 in overflow bucket
}

TEST_F(HistogramTest, ScopedTimer) {
    {
        auto timer = histogram->scoped();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(histogram->getCount(), 1);
    EXPECT_GT(histogram->getSum(), 0.0);
}
