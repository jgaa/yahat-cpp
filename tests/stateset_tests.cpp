 #include <gtest/gtest.h>
#include "yahat/Metrics.h"  // Ensure this includes the Stateset class

using namespace yahat;

class StatesetTest : public ::testing::Test {
protected:
    void SetUp() override {
        yahat::Metrics::labels_t labels = { {"module", "test"} };
        std::vector<std::string> states = {"starting", "running", "stopped"};
        stateset = metrics.AddStateset<3>("service_state", "State of the service", "", labels, states);
    }

    Metrics metrics;
    Metrics::Stateset<3>* stateset{};
};

TEST_F(StatesetTest, InitialState) {
    EXPECT_FALSE(stateset->getState(0));
    EXPECT_FALSE(stateset->getState(1));
    EXPECT_FALSE(stateset->getState(2));
}

TEST_F(StatesetTest, SetStateByIndex) {
    stateset->setState(1, true);
    EXPECT_FALSE(stateset->getState(0));
    EXPECT_TRUE(stateset->getState(1));
    EXPECT_FALSE(stateset->getState(2));
}

TEST_F(StatesetTest, SetStateByName) {
    stateset->setState("running", true);
    EXPECT_FALSE(stateset->getState("starting"));
    EXPECT_TRUE(stateset->getState("running"));
    EXPECT_FALSE(stateset->getState("stopped"));
}

TEST_F(StatesetTest, SetExclusiveState) {
    stateset->setExclusiveState(2);
    EXPECT_FALSE(stateset->getState(0));
    EXPECT_FALSE(stateset->getState(1));
    EXPECT_TRUE(stateset->getState(2));
}

TEST_F(StatesetTest, InvalidStateName) {
    EXPECT_THROW(stateset->setState("unknown", true), std::out_of_range);
    EXPECT_THROW(stateset->getState("unknown"), std::out_of_range);
}

TEST_F(StatesetTest, InvalidStateIndex) {
    EXPECT_THROW(stateset->setState(5, true), std::out_of_range);
    EXPECT_THROW(stateset->getState(5), std::out_of_range);
}
