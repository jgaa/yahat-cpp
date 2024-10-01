#pragma once

#include <map>
#include <string>
#include <array>

#include "yahat/config.h"

#ifdef YAHAT_ENABLE_METRICS

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

    /*! Constructor
     *
     *  @param metrics Pointer to an existing Metrics instance. If nullptr, a new instance will be created.
     */
    YahatInstanceMetrics(Metrics * metrics = {});

    Metrics& metrics() {
        assert(metrics_ != nullptr);
        return *metrics_;
    }

    counter_t * incomingRequests() { return incoming_requests_; }
    counter_t * tcpConnections() { return tcp_connections_; }
    gauge_t * currentSessions() { return current_sessions_; }
    counter_t * httpRequests(const std::string& route);
    gauge_t * workerThreads() { return worker_threads_; }

    HttpServer::handler_t metricsHandler();

    void addHttpRequests(std::string_view target, const std::span<std::string_view> methods);

    void incrementHttpRequestCount(const std::string_view route, std::string_view method);

    using gauge_scoped_t = Metrics::Scoped<gauge_t>;
    using counter_scoped_t = Metrics::Scoped<counter_t>;

private:
    Metrics *metrics_{};
    std::unique_ptr<Metrics> metrics_instance_; // If we need to allocate it ourselves
    counter_t * incoming_requests_{};
    counter_t * tcp_connections_{};
    gauge_t * current_sessions_{};
    gauge_t * worker_threads_{};
    std::map<std::string, counter_t *> http_requests_; // Count of requests per route

    alignas(cache_line_size_) std::mutex mutex_;
    char mpadding_[cache_line_size_ - sizeof(std::mutex)];
};

}

#endif // YAHAT_ENABLE_METRICS
