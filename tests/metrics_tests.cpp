
#include <iostream>
#include <thread>
#include <chrono>

#include "gtest/gtest.h"

#include "yahat/Metrics.h"
#include "yahat/logging.h"

using namespace std;
using namespace std::chrono_literals;
using namespace yahat;

static const auto test_time = chrono::system_clock::from_time_t(1727625364) + 124ms;

TEST (Metrics, Counter) {
    Metrics metrics;
    metrics.setNow(test_time);

    auto *counter = metrics.AddCounter("http_requests", "Number of http-requests", "", Metrics::labels_t{{"method", "GET"}, {"endpoint", "/"}});

    EXPECT_EQ(counter->type(), Metrics::DataType::Type::Counter);
    EXPECT_EQ(counter->name(), "http_requests");
    EXPECT_EQ(counter->help(), "Number of http-requests");
    EXPECT_EQ(counter->unit(), "");
    EXPECT_EQ(counter->metricName(), "http_requests{endpoint=\"/\",method=\"GET\"}");
    EXPECT_EQ(counter->value(), 0);

    counter->inc();
    EXPECT_EQ(counter->value(), 1);
    counter->inc(2);
    EXPECT_EQ(counter->value(), 3);

    {
        std::ostringstream target;
        metrics.generate(target);

        const auto expected = R"(# HELP http_requests Number of http-requests
# TYPE http_requests counter
http_requests_total{endpoint="/",method="GET"} 3
http_requests_created{endpoint="/",method="GET"} 1727625364.124
)";

        EXPECT_EQ(target.str(), expected);
    }

    auto second = metrics.clone(*counter, Metrics::labels_t{{"method", "GET"}, {"endpoint", "/metrics"}});

    EXPECT_EQ(second->type(), Metrics::DataType::Type::Counter);
    EXPECT_EQ(second->name(), "http_requests");
    EXPECT_EQ(second->help(), "Number of http-requests");
    EXPECT_EQ(second->unit(), "");
    EXPECT_EQ(second->metricName(), "http_requests{endpoint=\"/metrics\",method=\"GET\"}");
    EXPECT_EQ(second->value(), 0);

    second->inc();

    {

    const auto expected = R"(# HELP http_requests Number of http-requests
# TYPE http_requests counter
http_requests_total{endpoint="/",method="GET"} 3
http_requests_created{endpoint="/",method="GET"} 1727625364.124
http_requests_total{endpoint="/metrics",method="GET"} 1
http_requests_created{endpoint="/metrics",method="GET"} 1727625364.124
)";

        std::ostringstream target;
        metrics.generate(target);

        EXPECT_EQ(target.str(), expected);
    }

    auto third = metrics.AddCounter("UDP_requests", "Number of udp-requests", "", Metrics::labels_t{{"method", "OPTIONS"}, {"endpoint", "/foo"}});
    third->inc(5);

    {
        const auto expected = R"(# HELP UDP_requests Number of udp-requests
# TYPE UDP_requests counter
UDP_requests_total{endpoint="/foo",method="OPTIONS"} 5
UDP_requests_created{endpoint="/foo",method="OPTIONS"} 1727625364.124
# HELP http_requests Number of http-requests
# TYPE http_requests counter
http_requests_total{endpoint="/",method="GET"} 3
http_requests_created{endpoint="/",method="GET"} 1727625364.124
http_requests_total{endpoint="/metrics",method="GET"} 1
http_requests_created{endpoint="/metrics",method="GET"} 1727625364.124
)";

        std::ostringstream target;
        metrics.generate(target);
        EXPECT_EQ(target.str(), expected);
    }

}

TEST (Metrics, lookup) {
    Metrics metrics;
    auto *http_req = metrics.AddCounter("http_requests", "Number of http-requests", "", Metrics::labels_t{{"method", "GET"}, {"endpoint", "/"}});
    auto *tcp_sockets = metrics.AddCounter("http_tcp_sockets", "Number of unique TCP sockets used by HTTP requests");

    EXPECT_EQ(metrics.lookup("http_requests", Metrics::labels_t{{"endpoint", "/"}, {"method", "GET"}}), http_req);
    EXPECT_EQ(metrics.lookup("http_requests", Metrics::labels_t{{"endpoint", "/"}, {"method", "POST"}}), nullptr);
    EXPECT_EQ(metrics.lookup("http_tcp_sockets"), tcp_sockets);
    EXPECT_EQ(metrics.lookup("http_tcp_sockets", Metrics::labels_t{{"foo", "bar"}}), nullptr);
}

TEST (Metrics, lookupInfo) {
    // Info is special, because it's key has a '#' prefix (to sort it on top), which makes its key different from its metricName()
    Metrics metrics;
    auto *build = metrics.AddInfo("build", "Build information", "", Metrics::labels_t{{"version", "1.0.0"}});
    auto *version = metrics.AddInfo("version", "Version information", "", Metrics::labels_t{{"version", "1.0.0"}});
    auto *version2 = metrics.AddInfo("version", "Version information", "", Metrics::labels_t{{"version", "2.0.0"}});
    auto *version3 = metrics.AddInfo("version", "Version information", "", Metrics::labels_t{{"version", "3.0.0"}});

    EXPECT_EQ(metrics.lookup("build", Metrics::labels_t{{"version", "1.0.0"}}, Metrics::DataType::Type::Info), build);
    EXPECT_EQ(metrics.lookup("version", Metrics::labels_t{{"version", "1.0.0"}}, Metrics::DataType::Type::Info), version);
    EXPECT_EQ(metrics.lookup("version", Metrics::labels_t{{"version", "2.0.0"}}, Metrics::DataType::Type::Info), version2);
    EXPECT_EQ(metrics.lookup("version", Metrics::labels_t{{"version", "3.0.0"}}, Metrics::DataType::Type::Info), version3);
    EXPECT_EQ(metrics.lookup("version", Metrics::labels_t{{"version", "4.0.0"}}, Metrics::DataType::Type::Info), nullptr);
    EXPECT_EQ(metrics.lookup("build", Metrics::labels_t{{"version", "1.0.0"}}), nullptr);
}

TEST (Metrics, lokupWithType) {
    Metrics metrics;
    auto *build = metrics.AddInfo("build", "Build information", "", Metrics::labels_t{{"version", "1.0.0"}});
    auto *counter = metrics.AddCounter("http_requests", "Number of http-requests", "", Metrics::labels_t{{"method", "GET"}, {"endpoint", "/"}});
    auto *gauge = metrics.AddGauge("queue_entries", "Number entries in the queue", "count", Metrics::labels_t{{"method", "GET"}, {"endpoint", "/"}});

    EXPECT_EQ(metrics.lookup("build", Metrics::labels_t{{"version", "1.0.0"}}, Metrics::DataType::Type::Info), build);
    EXPECT_EQ(metrics.lookup("build", Metrics::labels_t{{"version", "1.0.0"}}, Metrics::DataType::Type::Gauge), nullptr);
    EXPECT_EQ(metrics.lookup("http_requests", Metrics::labels_t{{"method", "GET"}, {"endpoint", "/"}}, Metrics::DataType::Type::Counter), counter);
    EXPECT_EQ(metrics.lookup("http_requests", Metrics::labels_t{{"method", "GET"}, {"endpoint", "/"}}, Metrics::DataType::Type::Gauge), nullptr);
    EXPECT_EQ(metrics.lookup("queue_entries", Metrics::labels_t{{"method", "GET"}, {"endpoint", "/"}}, Metrics::DataType::Type::Gauge), gauge);
    EXPECT_EQ(metrics.lookup("queue_entries", Metrics::labels_t{{"method", "GET"}, {"endpoint", "/"}}, Metrics::DataType::Type::Counter), nullptr);
}

TEST(Metrics, Gauge) {
    Metrics metrics;
    metrics.setNow(test_time);

    auto *gauge = metrics.AddGauge("queue_entries", "Number entries in the queue", "count", Metrics::labels_t{{"method", "GET"}, {"endpoint", "/"}});

    EXPECT_EQ(gauge->type(), Metrics::DataType::Type::Gauge);
    EXPECT_EQ(gauge->name(), "queue_entries");
    EXPECT_EQ(gauge->help(), "Number entries in the queue");
    EXPECT_EQ(gauge->unit(), "count");
    EXPECT_EQ(gauge->metricName(), "queue_entries{endpoint=\"/\",method=\"GET\"}");
    EXPECT_EQ(gauge->value(), 0);

    gauge->set(100);
    EXPECT_EQ(gauge->value(), 100);
    gauge->set(123);
    EXPECT_EQ(gauge->value(), 123);

    {
        std::ostringstream target;
        metrics.generate(target);

        const auto expected = R"(# HELP queue_entries Number entries in the queue
# TYPE queue_entries gauge
# UNIT queue_entries count
queue_entries{endpoint="/",method="GET"} 123
queue_entries_created{endpoint="/",method="GET"} 1727625364.124
)";

        EXPECT_EQ(target.str(), expected);
    }

}

TEST(Metrics, Info) {
    Metrics metrics;
    metrics.setNow(test_time);

    auto *info = metrics.AddInfo("build", "Build information", "", Metrics::labels_t{{"version", "1.0.0"}});
    EXPECT_EQ(info->type(), Metrics::DataType::Type::Info);
    EXPECT_EQ(info->name(), "build");
    EXPECT_EQ(info->help(), "Build information");
    EXPECT_EQ(info->unit(), "");
    EXPECT_EQ(info->metricName(), "build{version=\"1.0.0\"}");

    {
        std::ostringstream target;
        metrics.generate(target);

        const auto expected = R"(# HELP build Build information
# TYPE build info
build_info{version="1.0.0"} 1
build_created{version="1.0.0"} 1727625364.124
)";
        EXPECT_EQ(target.str(), expected);
    }
}

TEST(Metrics, InfoComesFirst) {
    Metrics metrics;
    metrics.setNow(test_time);

    auto c1 = metrics.AddCounter("c1", "Counter 1", "", Metrics::labels_t{{"a", "1"}});
    auto c2 = metrics.AddCounter("c2", "Counter 2", "", Metrics::labels_t{{"a", "2"}});
    auto i1 = metrics.AddInfo("i1", "Info 1", "", Metrics::labels_t{{"a", "1"}});
    auto i2 = metrics.AddInfo("i2", "Info 2", "", Metrics::labels_t{{"a", "2"}});
    auto c3 = metrics.AddCounter("c3", "Counter 3", "", Metrics::labels_t{{"a", "3"}});

    {
        std::ostringstream target;
        metrics.generate(target);

        const auto expected = R"(# HELP i1 Info 1
# TYPE i1 info
i1_info{a="1"} 1
i1_created{a="1"} 1727625364.124
# HELP i2 Info 2
# TYPE i2 info
i2_info{a="2"} 1
i2_created{a="2"} 1727625364.124
# HELP c1 Counter 1
# TYPE c1 counter
c1_total{a="1"} 0
c1_created{a="1"} 1727625364.124
# HELP c2 Counter 2
# TYPE c2 counter
c2_total{a="2"} 0
c2_created{a="2"} 1727625364.124
# HELP c3 Counter 3
# TYPE c3 counter
c3_total{a="3"} 0
c3_created{a="3"} 1727625364.124
)";
        EXPECT_EQ(target.str(), expected);
    }
}

TEST(Metrics, Clone) {
    Metrics metrics;
    metrics.setNow(test_time);

    auto *gauge = metrics.AddGauge("queue_entries", "Number entries in the queue", "count", Metrics::labels_t{{"method", "GET"}, {"endpoint", "/"}});
    gauge->set(1);
    auto info = metrics.AddInfo("build", "Build information", "", Metrics::labels_t{{"version", "1.0.0"}});

    auto * cloned_gauge = metrics.clone(*gauge, Metrics::labels_t{{"method", "POST"}, {"endpoint", "/cloned"}});
    auto * cloned_info = metrics.clone(*info, Metrics::labels_t{{"version", "cloned-2.0.0"}});

    {
        std::ostringstream target;
        metrics.generate(target);

        const auto expected = R"(# HELP build Build information
# TYPE build info
build_info{version="1.0.0"} 1
build_created{version="1.0.0"} 1727625364.124
build_info{version="cloned-2.0.0"} 1
build_created{version="cloned-2.0.0"} 1727625364.124
# HELP queue_entries Number entries in the queue
# TYPE queue_entries gauge
# UNIT queue_entries count
queue_entries{endpoint="/",method="GET"} 1
queue_entries_created{endpoint="/",method="GET"} 1727625364.124
queue_entries{endpoint="/cloned",method="POST"} 0
queue_entries_created{endpoint="/cloned",method="POST"} 1727625364.124
)";

        EXPECT_EQ(target.str(), expected);
    }
}

TEST(Metrics, CloneDuplicateLabels) {
    Metrics metrics;
    metrics.setNow(test_time);

    auto *gauge = metrics.AddGauge("queue_entries", "Number entries in the queue", "count", Metrics::labels_t{{"method", "GET"}, {"endpoint", "/"}});

    EXPECT_THROW(metrics.clone(*gauge, gauge->labels()), std::invalid_argument);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

#ifdef USE_LOGFAULT
    auto llevel = logfault::LogLevel::INFO;
    if (log_level == "debug") {
        llevel = logfault::LogLevel::DEBUGGING;
    } else if (log_level == "trace") {
        llevel = logfault::LogLevel::TRACE;
    } else if (log_level == "info") {
        ;  // Do nothing
    } else {
        std::cerr << "Unknown log-level: " << log_level << endl;
        return -1;
    }

    logfault::LogManager::Instance().AddHandler(
        make_unique<logfault::StreamHandler>(clog,  logfault::LogLevel::INFO));
#else
    Logger::Instance().SetLogLevel(LogLevel::INFO);
    Logger::Instance().SetHandler([](LogLevel level, const std::string& msg) {
        static const std::array<std::string, 6> levels = {"NONE", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"};

        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

        std::clog << std::put_time(std::localtime(&now), "%c") << ' '
                  << levels.at(static_cast<size_t>(level))
                  << ' ' << std::this_thread::get_id() << ' '
                  << msg << std::endl;
    });
#endif

    return RUN_ALL_TESTS();
}
