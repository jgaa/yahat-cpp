#include <gtest/gtest.h>
#include "yahat/Metrics.h"  // Ensure this includes the Counter class

using namespace yahat;

class CounterTest : public ::testing::Test {
protected:
    void SetUp() override {
        counter = metrics.AddCounter("request_count", "Total number of requests", "requests", {});
    }

    Metrics metrics;
    Metrics::Counter<uint64_t>* counter{};
};

TEST_F(CounterTest, InitialState) {
    EXPECT_EQ(counter->value(), 0);
}

TEST_F(CounterTest, IncrementByOne) {
    counter->inc();
    EXPECT_EQ(counter->value(), 1);
}

TEST_F(CounterTest, IncrementByValue) {
    counter->inc(5);
    EXPECT_EQ(counter->value(), 5);
}

TEST_F(CounterTest, MultipleIncrements) {
    counter->inc(2);
    counter->inc(3);
    EXPECT_EQ(counter->value(), 5);
}
