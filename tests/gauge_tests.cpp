#include <gtest/gtest.h>
#include "yahat/Metrics.h"  // Ensure this includes the Gauge class

using namespace yahat;

class GaugeTest : public ::testing::Test {
protected:
    void SetUp() override {
        gauge = metrics.AddGauge("temperature", "Current temperature", "celsius", {});
    }

    Metrics metrics;
    Metrics::Gauge<uint64_t>* gauge{};
};

TEST_F(GaugeTest, InitialState) {
    EXPECT_EQ(gauge->value(), 0);
}

TEST_F(GaugeTest, SetValue) {
    gauge->set(42);
    EXPECT_EQ(gauge->value(), 42);
}

TEST_F(GaugeTest, IncrementValue) {
    gauge->inc(5);
    EXPECT_EQ(gauge->value(), 5);
}

TEST_F(GaugeTest, DecrementValue) {
    gauge->set(10);
    gauge->dec(3);
    EXPECT_EQ(gauge->value(), 7);
}

TEST_F(GaugeTest, ScopedGauge) {
    {
        auto scoped = gauge->scoped();
        gauge->inc(20);
    }
    EXPECT_EQ(gauge->value(), 20);
}
