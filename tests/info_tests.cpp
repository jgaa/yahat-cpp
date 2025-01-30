#include <gtest/gtest.h>
#include <sstream>
#include "yahat/Metrics.h"  // Ensure this includes the Info class

using namespace yahat;

class InfoTest : public ::testing::Test {
protected:
    void SetUp() override {
        info = metrics.AddInfo("build_version", "Software build version", "", {{"version", "1.2.3"}});
    }

    Metrics metrics;
    Metrics::Info* info{};
};

TEST_F(InfoTest, RenderOutput) {
    std::ostringstream output;
    info->render(output);
    std::string rendered = output.str();
    EXPECT_NE(rendered.find("build_version_info{version=\"1.2.3\"}"), std::string::npos);
    EXPECT_NE(rendered.find("1"), std::string::npos);
}
