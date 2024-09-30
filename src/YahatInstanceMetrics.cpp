#include "yahat/config.h"
#ifdef YAHAT_ENABLE_METRICS

#include <array>
#include <format>

#include "yahat/YahatInstanceMetrics.h"

using namespace std;

namespace yahat {

namespace {

class MetricsHandler : public RequestHandler {
    // RequestHandler interface
public:
    MetricsHandler(YahatInstanceMetrics& metrics)
        : metrics_(metrics)
    {
    }

    Response onReqest(const Request &req) override {
        if (req.type != Request::Type::GET) {
            return {405, "Method Not Allowed - only GET is allowed here"};
        }

        std::string body;
        {
            ostringstream response;
            metrics_.metrics().generate(response);
            body = response.str();
        }

        return {200, "OK", body, {}, metrics_.metrics().contentType()};
    }

private:
    YahatInstanceMetrics& metrics_;
};

} // anon ns


YahatInstanceMetrics::YahatInstanceMetrics() {

    incoming_requests_ = metrics_.AddCounter<uint64_t>("yahat_incoming_requests", "Number of incoming requests", "count");
    tcp_connections_ = metrics_.AddCounter<uint64_t>("yahat_tcp_connections", "Number of TCP connections", "count");
    current_sessions_ = metrics_.AddGauge<uint64_t>("yahat_current_sessions", "Number of current sessions", "count");
    worker_threads_ = metrics_.AddGauge<uint64_t>("yahat_worker_threads", "Number of worker threads", "count");

    addHttpRequests("/", {});
}

yahat::HttpServer::handler_t YahatInstanceMetrics::metricsHandler()
{

    return make_shared<MetricsHandler>(*this);
}

void YahatInstanceMetrics::addHttpRequests(const std::string &route, std::span<std::string_view> methods)
{
    static constexpr auto all_methods = to_array<string_view>({"GET", "PUT", "POST", "PATCH", "DELETE", "OPTIONS", "O"});

    auto add = [&](auto& range) {
        lock_guard lock{mutex_};
        for (const auto& method : range) {
            const auto key = format("{}{}", method, route);
            http_requests_[key] = metrics_.AddCounter<uint64_t>(
                "yahat_incoming_requests", "Number of incoming requests",
                "count", {{"route", route}, {"method", string{method}}});
        }
    };

    if (methods.empty()) {
        add(methods);
        return;
    }

    add(all_methods);
}

void YahatInstanceMetrics::incrementHttpRequestCount(const std::string_view route, std::string_view method)
{
    const auto key = format("{}{}", method, route);
    const auto default_key = format("O{}", route);

    {
        // TODO: Optimize here. See if read/write mutexes are faster in this case.
        //       If not, use sharded mutexes.
        //       Or better, keep a pointer in the handler itself.
        lock_guard lock{mutex_};
        if (auto it = http_requests_.find(key); it != http_requests_.end()) {
            it->second->inc();
        } else if (auto dit = http_requests_.find(key); dit != http_requests_.end()) {
            dit->second->inc();
        }
    }
}

}  // ns

#endif
