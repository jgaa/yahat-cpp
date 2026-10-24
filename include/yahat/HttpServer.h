#pragma once

#include <map>
#include <functional>
#include <filesystem>
#include <string_view>
#include <future>
#include <span>
#include <memory>
#include <queue>
#include <atomic>
#include <optional>
#include <any>
#include <algorithm>
#include <cstdint>
#include <vector>
#include <mutex>

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/version.hpp>
#include <boost/beast/http.hpp>

#if BOOST_VERSION >= 107500
#   define USING_BOOST_JSON
#   include <boost/json.hpp>
#endif

#if BOOST_VERSION >= 107500
#   define USING_BOOST_URL
#   include <boost/url.hpp>
#endif

#include "yahat/config.h"

#ifdef YAHAT_ENABLE_METRICS
#   include "yahat/Metrics.h"
#endif

// Usual noise to compile under Windows
#ifdef min
#   undef min
#endif
#ifdef max
#   undef max
#endif
#ifdef DELETE
#   undef DELETE
#endif
#ifdef ERROR
#   undef ERROR
#endif
#ifdef DEBUG
#   undef DEBUG
#endif

namespace yahat {

using uint = unsigned int;

class YahatInstanceMetrics;
class Metrics;
class HttpServer;

struct HttpConfig {
    /*! Number of threads for the API and UI.
     *  Note that db and file access
     *  is syncronous, so even if the HTTP server is
     *  asyncroneous, we need some
     *  extra threads to wait for slow IO to complete.
     */
    size_t num_http_threads = 6;

    /*! Ip address or hostname for the REST API endpoint */
    std::string http_endpoint;

    /*! HTTP port
     *
     *  Only required for non-standard ports
     */
    std::string http_port;

    /*! Path to the TLS key-file if HTTPS is used */
    std::string http_tls_key;
    /*! Path to the TLS cert-file if HTTPS is used */
    std::string http_tls_cert;

    /*! Enables full support for HTTP Basic  Authentication.
     *
     *  See:https://developer.mozilla.org/en-US/docs/Web/HTTP/Authentication
     *
     *  Note that the application must still provide a auth functor that
     *  performs the actual authentication
     */
    bool enable_http_basic_auth = true;

    std::string http_basic_auth_realm;

    /*! IO timeout in seconds for requests in/out */
    unsigned http_io_timeout = 120;

    /*! Maximum size for a compressed request */
    uint max_decompressed_size = 10 * 1024 * 1024; // 10 MB

    /*! CORS is a resposne to Javascript madness.
     *
     *  In an API server, the normal handling is usually
     *  to ignore it and just answer "yea, right, whatever you want..."
     *  which is to send a HTTP 200 message and the headers:
     *   -  Access-Control-Allow-Origin: *
     *   -  Access-Control-Allow-Credentials: true
     *   -  Access-Control-Allow-Methods: GET,OPTIONS,POST,PUT,PATCH,DELETE
     *   -  Access-Control-Allow-Headers: *
     */
    bool auto_handle_cors = true;

#ifdef YAHAT_ENABLE_METRICS
    /*! Enable metrics for this server
     *
     *  Metrics are available at /metrics
     */
    bool enable_metrics = true;

    std::string metrics_target = "/metrics";
#endif

    // Unset preserves Boost.Beast's existing parser limits.
    std::optional<uint64_t> max_request_body_size;
    std::optional<uint32_t> max_request_header_size;
};

boost::uuids::uuid generateUuid();

/*! Data returned by the authenticator */
struct Auth {
    std::string account;
    bool access = false;
    int rcode = 0;

    /// Optional data the application can set for it's own use
    std::any extra;
};

struct Request : public std::enable_shared_from_this <Request>{
    enum class Type {
        GET,
        PUT,
        PATCH,
        POST,
        DELETE,
        OPTIONS
    };

    Request() = default;

    Request(std::string undecodedTtarget,
            std::string body,
            Type type,
            boost::asio::yield_context *yield,
            bool isTls = false)
        : body{std::move(body)}
        , type{type}, yield{yield}
        , is_https{isTls} {
        init(undecodedTtarget);
    }

    void init(const std::string& undecodedTtarget);

    bool isHttps() const noexcept {
        return is_https;
    }

    std::string target;
    std::string_view route; // The part of the target that was matched by the chosen route.
    std::string body;
    Type type = Type::GET;
    boost::uuids::uuid uuid = generateUuid();
    Auth auth;
    boost::asio::yield_context *yield = {};
    std::string all_arguments;
    std::map<std::string_view, std::string_view> arguments;
    std::vector<std::pair<std::string_view, std::string_view>> cookies;
    bool is_https{false};

    /*! Check if the connection is still open to the client.
     */
    std::function<bool()> probe_connection_ok;

    /*! Can be set by a handler to allow the HTTP server to notify
     *  directly when a connection is closed.
     *
     *  Muteable because Requests in general are const
     */
    mutable std::function<void()> notify_connection_closed;

    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);

    bool isCancelled() const noexcept { return *cancelled; }

    // Owned headers remain valid for continuations after parsing completes.
    boost::beast::http::fields headers;

    std::string_view getHeader(std::string_view name) const noexcept {
        const auto it = headers.find({name.data(), name.size()});
        if (it == headers.end()) {
            return {};
        }
        return {it->value().data(), it->value().size()};
    }

    bool expectBody() const noexcept {
        return type == Type::POST || type == Type::PUT || type == Type::PATCH;
    }

    std::string_view getCookie(std::string_view name) const noexcept{
        if (auto it = std::find_if(cookies.begin(), cookies.end(), [name](const auto& p) { return p.first == name; });
            it != cookies.end()) {
            return it->second;
        }
        return {};
    }

    std::string_view getArgument(std::string_view name) const noexcept {
        if (auto it = arguments.find(name); it != arguments.end()) {
            return it->second;
        }
        return {};
    }

#ifdef YAHAT_ENABLE_METRICS
    mutable std::optional<Metrics::ScopedTimer<Metrics::Histogram<double>, double>> requestDuration;
#endif
};

class Continuation;

struct Response {
    enum class Compression {
        NONE,
        GZIP
    };

    Response() = default;
    Response(int code, std::string reason)
        : code{code}, reason{std::move(reason)} {}
    Response(int code, std::string reason, std::string &&body)
        : code{code}, reason{std::move(reason)}, body{std::move(body)} {}
    Response(int code, std::string reason, std::string_view body)
        : code{code}, reason{std::move(reason)}, body{std::string{body}} {}
    Response(int code, std::string reason, std::string && body, std::string target)
        : code{code}, reason{std::move(reason)}, body{std::move(body)}, target{std::move(target)} {}
    Response(int code, std::string reason, std::string_view body, std::string target)
        : code{code}, reason{std::move(reason)}, body{body}, target{std::move(target)} {}
    Response(int code, std::string reason, std::string && body, std::string target, std::string_view mime_type)
        : code{code}, reason{std::move(reason)}, body{std::move(body)}, target{std::move(target)}, mime_type{mime_type} {}
    Response(int code, std::string reason, std::string_view body, std::string target, std::string_view mime_type)
        : code{code}, reason{std::move(reason)}, body{body}, target{std::move(target)}, mime_type{mime_type} {}

    int code = 200;
    std::string reason = "OK";
    std::string body;
    std::string target; // The actual target
    std::string_view mime_type;
    std::string_view mimeType() const;
    static std::string_view getMimeType(std::string_view type = "json");
    bool close = false;
    mutable bool cors = false;
    mutable Compression compression = Compression::NONE;
    std::vector<std::pair<std::string, std::string>> cookies;

    // Legacy empty-body status JSON remains the default.
    bool generate_status_body = true;
    boost::beast::http::fields headers;
    std::optional<bool> auto_handle_cors;

    // Copies application headers, rejecting invalid values and framing headers.
    void applyHeaders(boost::beast::http::fields& target) const;

    bool ok() const noexcept {
        return code / 100 == 2;
    }

    std::string responseStatusAsJson() const {
#ifdef USING_BOOST_JSON
        boost::json::object o;
        o["error"] = code / 100 > 2;
        o["status"] = code;
        o["reason"] = reason;
        return boost::json::serialize(o);
#else
        return {};
#endif
    }

    std::shared_ptr<Continuation> continuation() {
        if (cont_) {
            return std::move(cont_);
        }
        return {};
    }

    void setContinuation(std::shared_ptr<Continuation> cont) {
        cont_ = std::move(cont);
    }

private:
    std::shared_ptr<Continuation> cont_;
};

/*! Configure routes before starting the server. Callbacks must be thread safe.
 * Filters run before authentication, including for OPTIONS. Public routes
 * explicitly bypass authentication. Defaults preserve legacy behavior.
 */
struct RouteOptions {
    bool public_access = false;
    bool exact_match = false;
    std::optional<bool> auto_handle_cors;
    // Unset accepts any origin; an empty list rejects every present Origin.
    std::optional<std::vector<std::string>> allowed_origins;
    std::vector<Request::Type> allowed_methods;
    std::function<std::optional<Response>(const Request&)> request_filter;
    std::function<Response(const Request&)> unauthorized_response;
};

class Continuation {
public:
    class Stream {
    public:
        using chunk_t = decltype(boost::beast::http::make_chunk(boost::asio::const_buffer{}));

        virtual ~Stream() = default;

        virtual size_t async_read_stream(boost::asio::mutable_buffer buffer) = 0;

        virtual void async_read_stream(boost::asio::mutable_buffer buffer,
                                       std::function<void(const boost::system::error_code&, std::size_t)> handler) = 0;

        virtual bool async_write_stream(boost::asio::const_buffer buffer) = 0;

        virtual bool async_write_stream(const chunk_t& chunk) = 0;

        // Default preserves source compatibility with existing custom Streams.
        virtual bool finish_stream() { return false; }

        virtual bool async_write_header(boost::beast::http::response_serializer<boost::beast::http::empty_body>& ser) = 0;

        virtual void set_timeout(std::chrono::seconds timeout) = 0;

        virtual void disable_timeout() = 0;
    };


    Continuation() = default;
    virtual ~Continuation() = default;

    // Called before proceed(); existing continuations need not override it.
    virtual void prepareResponse(const Response&) {}

    virtual Response proceed(Stream& stream, boost::asio::yield_context& yield) = 0;
};


/*! Basic SSE handler where the app can implement the actual work-flow */
struct SseHandler : public Continuation {

    // Prevent copy and move
    SseHandler(HttpServer& server)
        : server_{server} {}

    SseHandler(const SseHandler&) = delete;
    SseHandler(SseHandler&&) = delete;
    SseHandler & operator=(const SseHandler&) = delete;
    SseHandler & operator=(SseHandler&&) = delete;


    /*! Send function type.
     *
     *  Note that the send function is a stateful co-routine
     *  an will suspend and resume as needed using the
     *  HTTP sessions yield context.
     */

    Response proceed(Stream& stream, boost::asio::yield_context& yield) override {
        stream_ = &stream;
        yield_ = &yield;
        auto response = proceeding();
        if (sse_initialized_ && !eos_data_->closed) {
            stream.finish_stream();
        }
        return response;
    }

    // Set before proceed(). Applies to GET and POST response streams.
    boost::beast::http::fields headers;

    void prepareResponse(const Response& response) override;

protected:
    /*! Called by the server to allow the handler to proceed with the SSE stream.
     *
     *  This function is called by the server after the handler has been called
     *  and the handler has set up the send function.
     *
     *  The handler can now start sending messages to the client.
     */
    virtual Response proceeding() = 0;

    // Send a sse message.
    bool send(std::string_view message);

    auto& getYield() const noexcept {
        assert(yield_);
        return *yield_;
    }

    auto& server() noexcept {
        return server_;
    }

    void setOnConnectionClosed(std::function<void()> cb) {
        eos_data_->notify_connection_closed = std::move(cb);
    }

private:
    HttpServer& server_;
    struct EosData {
        std::array<char, 1> buffer;
        std::atomic_bool closed{false};
        std::function<void()> notify_connection_closed;
    };
    std::shared_ptr<EosData> eos_data_ = std::make_shared<EosData>();
    Stream *stream_{};
    boost::asio::yield_context *yield_{};
    bool sse_initialized_{false};
};

/*! SSE handler where an app can append messages to a queue to send to the client
 *
 *  Convenience class for handling SSE streams
 */
class SseQueueHandler : public SseHandler {
public:
    SseQueueHandler(HttpServer& server);
    ~SseQueueHandler() override;

    /*! Sends a raw SSE message.
     *
     *  The message must be formatted according to the SSE requirements.
     *
     *  An empty message will no be sent, but it will initialize the SSE connection
     *  if it is unilinialized. This is useful to send the headers to the client.
     */
    void sendSse(std::string message);

    /*! High level function to send a message to the client.
     *
     *  This function will format the message according to the SSE requirements.
     *
     *  @param eventName The event name to send.
     *         Cannot contain newlines. Cannot be empty.
     *  @param data The data to send. Cannot contain newlines.
     *         This is often a json string.
     */
    void sendSse(std::string_view eventName, std::string_view data);

    bool active() const noexcept;

    // Zero preserves the legacy unlimited queue. Overflow throws length_error.
    void setQueueLimit(size_t max_bytes);
    // Zero disables heartbeat comments (the legacy behavior).
    void setHeartbeatInterval(std::chrono::seconds interval);

    void closeSse();
    // Drain queued messages, then terminate the chunked response.
    void finishSse();

protected:
    Response proceeding() override;

private:
    std::optional<std::string> next();

    struct QueueState;
    std::shared_ptr<QueueState> state_;
};

struct AuthReq {
    AuthReq(const Request& req, boost::asio::yield_context& yield)
        : req{req}, yield{yield} {}

    std::string_view auth_header;
    const Request& req;
    boost::asio::yield_context& yield;
};

/*! Authenticator
 *
 *  This method is called for each request to handle
 *  authentication
 */
using authenticator_t = std::function<Auth(const AuthReq&)>;


class RequestHandler {
public:
    virtual ~RequestHandler() = default;

    /*! Request handler
     *
     *  This method is called on the handler with the best mathing
     *  route.
     *
     *  \param req Request to process
     *  \param auth Authentication for the request.
     *
     *  \throws std::exception on error
     *  \throws Response if an internal state triggered an exception
     *          that provided a valid response. This may be an error,
     *          or a shortcut to exit further processing in the handler.
     */
    virtual Response onReqest(const Request& req) = 0;

#ifdef YAHAT_ENABLE_METRICS
    /*! Enable metrics for this handler
     *
     *  When enabled, the server will produce metrics for al requests from this handler.
     *
     *  @param target The target for the handler. This is used as a label for the metrics.
     */
    void enableMetrics(HttpServer& server, std::string_view target);

    /*! Used by the server to handle the metrics */
    auto *metrics() noexcept {
        return metrics_;
    }

private:
    Metrics::Histogram<double> * metrics_{};
#endif
};

template <typename T>
class EmbeddedHandler : public RequestHandler {
public:
    EmbeddedHandler(const T& content, std::string prefix)
        : content_{content}, prefix_{std::move(prefix)} {}

    Response onReqest(const Request& req) override {
        // Remove prefix
        auto t = std::string_view{req.target};
        if (t.size() < prefix_.size()) {
            throw std::runtime_error{"Invalid targert. Cannot be shorted than prefix!"};
        }

        t = t.substr(prefix_.size());

        while(!t.empty() && t.front() == '/') {
            t = t.substr(1);
        }

        if (t.empty()) {
            t = {"index.html"};
        }

        if (auto it = content_.find(std::string{t}) ; it != content_.end()) {
            std::filesystem::path served = prefix_;
            served /= t;

            return {200, "OK", std::string{it->second}, served.string()};
        }
        return {404, "Document not found"};
    }

private:
    const T& content_;
    const std::string prefix_;
};

// Very general HTTP server...
class HttpServer
{
public:
    using handler_t = std::shared_ptr<RequestHandler>;//std::function<Response (const Request& req)>;

    HttpServer(const HttpConfig& config, authenticator_t authHandler, const std::string& branding = {});

#ifdef YAHAT_ENABLE_METRICS
    /*! Constructor
     *
     *  @param config Configuration for the HTTP server
     *  @param authHandler Authentication handler
     *  @param metricsInstance Metrics instance to use. This must remain valid for the lifetime of the HTTP server
     *  @param branding Branding string to use for the server id
     */
    HttpServer(const HttpConfig& config, authenticator_t authHandler, Metrics& metricsInstance, const std::string& branding = {});
#endif

    /*! Starts the server and returns immediately */
    std::future<void> start();

    /*! Starts the server ahd returns when the server is finished
     *
     *  Alternative to `start`.
     */
    void run();

    void stop();

#ifdef YAHAT_ENABLE_METRICS
    /*! Get the metrics for this instance
     *
     *  You can use the metrics object to add your own metrics for your app.
     *
     *  @return The metrics object ot nullptr if metrics is disabled by configuration.
     */
    Metrics * metrics() noexcept;

    template <typename... T>
    void addRoute(std::string_view target, handler_t handler, T... methods)
    {
        std::array<std::string_view, sizeof...(T)> m = {methods...};
        addRoute_(target, handler, m);
    }

    void addRoute_(std::string_view target, handler_t handler, const std::span<std::string_view> metricMethods = {});
#else
    void addRoute(std::string_view target, handler_t handler);
#endif

    static std::string_view version() noexcept;

    void addRouteWithOptions(std::string_view target, handler_t handler, RouteOptions options);
    const RouteOptions& routeOptions(std::string_view target) const;

    std::pair<bool, std::string_view /* user name */> Authenticate(const std::string_view& authHeader);

    // Called by the HTTP server implementation template
    Response onRequest(Request& req) noexcept;

    // Serve a directory.
    // handles `index.html` by default. Lists the directory if there is no index.html.
    class FileHandler : public RequestHandler {
    public:
        FileHandler(std::filesystem::path root);

        Response onReqest(const Request &req) override;

        std::filesystem::path resolve(std::string_view target);
    private:
        Response readFile(const std::filesystem::path& path);
        Response handleDir(const std::filesystem::path& path);
        Response listDir(const std::filesystem::path& path);
        std::string getMimeType(const std::filesystem::path& path);

        const std::filesystem::path root_;
    };

    auto& getCtx() {
        return ctx_;
    }

    const auto& authenticator() const {
        return authenticator_;
    }

    std::string_view serverId() const noexcept {
        return server_;
    }

    auto& config() const noexcept {
        return config_;
    }

#ifdef YAHAT_ENABLE_METRICS
    auto * internalMetrics() noexcept {
        return metrics_.get();
    }

    const auto * internalMetrics() const noexcept {
        return metrics_.get();
    }
#endif

private:
    void startWorkers();
    std::map<std::string, handler_t>::const_iterator findRoute(std::string_view target) const;

    const HttpConfig& config_;
#ifdef YAHAT_ENABLE_METRICS
    std::shared_ptr<YahatInstanceMetrics> metrics_{};
#endif
    const authenticator_t authenticator_;
    boost::asio::io_context ctx_;
    std::map<std::string, handler_t> routes_;
    std::map<std::string, RouteOptions> route_options_;
    std::vector<std::thread> workers_;
    std::promise<void> promise_;
    const std::string server_;
};

} // ns
