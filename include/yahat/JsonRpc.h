#pragma once

#include "yahat/HttpServer.h"

#ifdef YAHAT_ENABLE_JSON_RPC
#include <shared_mutex>
#include <stdexcept>

namespace yahat {

/*! Transport-independent JSON-RPC 2.0 server dispatcher using Boost.JSON.
 * Methods may be registered concurrently. Callbacks execute without the registry
 * lock and must support concurrent invocation. Applications validate their own
 * parameter schemas and can throw Error to return a protocol error.
 */
class JsonRpc {
public:
    static constexpr int parse_error = -32700;
    static constexpr int invalid_request = -32600;
    static constexpr int method_not_found = -32601;
    static constexpr int invalid_params = -32602;
    static constexpr int internal_error = -32603;

    class Error : public std::runtime_error {
    public:
        Error(int code, std::string message, std::optional<boost::json::value> data = {});
        int code() const noexcept { return code_; }
        const auto& data() const noexcept { return data_; }
    private:
        int code_;
        std::optional<boost::json::value> data_;
    };

    struct Context {
        const Request* request = nullptr;
        boost::json::value id;
        bool notification = false;
    };

    struct Options {
        bool allow_batch = true;
        size_t max_batch_size = 128;
    };

    // params is null when omitted, otherwise an object or array.
    using method_t = std::function<boost::json::value(const boost::json::value&, const Context&)>;

    using async_method_t = std::function<boost::asio::awaitable<boost::json::value>(boost::json::value, Context)>;

    JsonRpc();
    explicit JsonRpc(Options options);
    void addMethod(std::string name, method_t handler);
    void addAsyncMethod(std::string name, async_method_t handler);
    // Owns messages and retains the optional HTTP request through suspension.
    boost::asio::awaitable<std::optional<boost::json::value>> dispatchAsync(
        boost::json::value message, std::shared_ptr<const Request> request = {}) const;
    bool removeMethod(std::string_view name);

    // No value means all valid messages were notifications; send no JSON body.
    std::optional<boost::json::value> dispatch(const boost::json::value& message,
                                             const Request* request = nullptr) const;
    std::optional<boost::json::value> handle(std::string_view json,
                                           const Request* request = nullptr) const;

    static boost::json::object result(boost::json::value id, boost::json::value value);
    static boost::json::object error(boost::json::value id, int code, std::string_view message,
                                     std::optional<boost::json::value> data = {});
private:
    std::optional<boost::json::value> dispatchOne(const boost::json::value& message,
                                                const Request* request) const;
    boost::asio::awaitable<std::optional<boost::json::value>> dispatchOneAsync(
        boost::json::value message, std::shared_ptr<const Request> request) const;
    Options options_;
    mutable std::shared_mutex mutex_;
    std::map<std::string, method_t, std::less<>> methods_;
    std::map<std::string, async_method_t, std::less<>> async_methods_;
};

/*! Basic POST/JSON adapter. MCP can reuse JsonRpc with its own transport rules.
 * Install with RouteOptions for Origin, authentication and CORS policies.
 */
class JsonRpcHandler : public RequestHandler {
public:
    explicit JsonRpcHandler(std::shared_ptr<JsonRpc> dispatcher);
    Response onReqest(const Request& request) override;
private:
    std::shared_ptr<JsonRpc> dispatcher_;
};

} // namespace yahat
#endif
