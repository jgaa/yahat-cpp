#pragma once

#include <map>
#include <string>
#include <array>

#include "yahat/config.h"
#include "yahat/HttpServer.h"
#include "yahat/Metrics.h"


namespace yahat
{

/*! Metrics for Yahat itself.
 *
 *  This class is responsible for providing metrics for the Yahat instance itself.
 */

class YahatInstanceMetrics
{
public:
    using counter_t = Metrics::Counter<uint64_t>;
    using gauge_t = Metrics::Gauge<uint64_t>;

    YahatInstanceMetrics();

    Metrics& metrics() { return metrics_; }

    counter_t * incomingrRequests() { return incoming_requests_; }
    counter_t * tcpConnections() { return tcp_connections_; }
    gauge_t * currentSessions() { return current_sessions_; }
    counter_t * httpRequests(const std::string& route);
    gauge_t * workerThreads() { return worker_threads_; }

    HttpServer::handler_t metricsHandler();

    void addHttpRequests(const std::string& route, std::span<std::string_view> methods);

    void incrementHttpRequestCount(const std::string_view route, std::string_view method);

private:
    Metrics metrics_;
    counter_t * incoming_requests_{};
    counter_t * tcp_connections_{};
    gauge_t * current_sessions_{};
    gauge_t * worker_threads_{};
    std::map<std::string, counter_t *> http_requests_; // Count of requests per route

    alignas(std::hardware_destructive_interference_size) std::mutex mutex_;
    char mpadding_[std::hardware_destructive_interference_size - sizeof(std::mutex)];
};

}
