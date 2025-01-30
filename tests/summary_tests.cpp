#include <gtest/gtest.h>
#include "yahat/Metrics.h"  // Ensure this includes the Summary class

#include <thread>
#include <chrono>

using namespace yahat;

class SummaryTest : public ::testing::Test {
protected:
    void SetUp() override {
        summary = metrics.AddSummary("response_time", "Response time distribution", "sec", {{"api", "test"}}, {0.5, 0.9, 0.99}, 10);
    }

    Metrics metrics;
    Metrics::Summary<double>* summary{};
};

TEST_F(SummaryTest, InitialState) {
    EXPECT_EQ(summary->getCount(), 0);
    EXPECT_DOUBLE_EQ(summary->getSum(), 0.0);
}

TEST_F(SummaryTest, ObserveSingleValue) {
    summary->observe(0.5);
    EXPECT_EQ(summary->getCount(), 1);
    EXPECT_DOUBLE_EQ(summary->getSum(), 0.5);
}

TEST_F(SummaryTest, ObserveMultipleValues) {
    summary->observe(0.2);
    summary->observe(0.5);
    summary->observe(0.8);

    EXPECT_EQ(summary->getCount(), 3);
    EXPECT_DOUBLE_EQ(summary->getSum(), 1.5);
}

TEST_F(SummaryTest, QuantileCalculation) {
    summary->observe(0.1);
    summary->observe(0.3);
    summary->observe(0.5);
    summary->observe(0.7);
    summary->observe(0.9);

    auto quantiles = summary->calculateQuantiles();
    ASSERT_EQ(quantiles.size(), 3);
    EXPECT_NEAR(quantiles[0], 0.5, 0.01); // p50
    EXPECT_NEAR(quantiles[1], 0.9, 0.01); // p90
    EXPECT_NEAR(quantiles[2], 0.9, 0.01); // p99
}

TEST_F(SummaryTest, RollingBuffer) {
    for (int i = 1; i <= 15; ++i) {
        summary->observe(i * 0.1);
    }
    EXPECT_EQ(summary->getCount(), 15);
    EXPECT_DOUBLE_EQ(summary->getSum(), 12.0);
}

TEST_F(SummaryTest, ScopedTimer) {
    {
        auto timer = summary->scoped();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_EQ(summary->getCount(), 1);
    EXPECT_GT(summary->getSum(), 0.0);
}
