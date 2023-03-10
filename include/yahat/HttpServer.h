#pragma once

#include <map>
#include <functional>
#include <filesystem>
#include <string_view>
#include <future>

#include <boost/asio.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/version.hpp>

#if BOOST_VERSION >= 107500
#   define USING_BOOST_JSON
#   include <boost/json.hpp>
#endif

#include "yahat/config.h"

namespace yahat {

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
};

boost::uuids::uuid generateUuid();

struct Request {
    enum class Type {
        GET,
        PUT,
        PATCH,
        POST,
        DELETE
    };

    boost::asio::yield_context *yield = {};
    std::string target;
    std::string_view route; // The part of the target that was matched by the chosen route.
    std::string auth; // from Authorization header
    std::string owner;
    std::string body;
    Type type = Type::GET;
    boost::uuids::uuid uuid = generateUuid();

    /*! Send one SSE event to the client.
     *
     *  @param sseEvent Complete and correctly formatted SSE event.
     *
     *  @exception std::exception if the operation fails.
     */
    std::function<void(std::string_view sseEvent)> sse_send;

    /*! Check if the connection is still open to the client.
     */
    std::function<bool()> probe_connection_ok;

    /*! Can be set by a handler to allow the HTTP server to notify
     *  directly when a connection is closed.
     *
     *  Muteable because Requests in general are const
     */
    mutable std::function<void()> notify_connection_closed;

    bool expectBody() const noexcept {
        return type == Type::POST || type == Type::PUT || type == Type::PATCH;
    }
};

struct Response {    
    int code = 200;
    std::string reason = "OK";
    std::string body;
    std::string target; // The actual target
    std::string_view mime_type;
    std::string_view mimeType() const;
    static std::string_view getMimeType(std::string_view type = "json");
    bool close = false;

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
};

/*! Data returned by the authenticator */
struct Auth {
    struct Extra {
        virtual ~Extra() = default;
    };

    std::string account;
    bool access = false;

    /// Optional data the application can set for it's own use
    std::unique_ptr<Extra> extra;
};

struct AuthReq {

    AuthReq(const Request& req) : req{req} {}

    std::string_view auth_header;
    const Request& req;
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
    virtual Response onReqest(const Request& req, const Auth& auth) = 0;
};

template <typename T>
class EmbeddedHandler : public RequestHandler {
public:
    EmbeddedHandler(const T& content, std::string prefix)
        : content_{content}, prefix_{std::move(prefix)} {}

    Response onReqest(const Request& req, const Auth& auth) override {
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

    /*! Starts the server and returns immediately */
    std::future<void> start();

    /*! Starts the server ahd returns when the server is finished
     *
     *  Alternative to `start`.
     */
    void run();

    void stop();

    void addRoute(std::string_view target, handler_t handler);

    static std::string_view version() noexcept;

    std::pair<bool, std::string_view /* user name */> Authenticate(const std::string_view& authHeader);

    // Called by the HTTP server implementation template
    Response onRequest(Request& req, const Auth& auth) noexcept;

    // Serve a directory.
    // handles `index.html` by default. Lists the directory if there is no index.html.
    class FileHandler : public RequestHandler {
    public:
        FileHandler(std::filesystem::path root);

        Response onReqest(const Request &req, const Auth& auth) override;

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

private:
    void startWorkers();

    const HttpConfig& config_;
    const authenticator_t authenticator_;
    std::map<std::string, handler_t> routes_;
    boost::asio::io_context ctx_;
    std::vector<std::thread> workers_;
    std::promise<void> promise_;
    const std::string server_;
};

} // ns
