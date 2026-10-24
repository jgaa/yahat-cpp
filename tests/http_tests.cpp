#include <gtest/gtest.h>
#include "yahat/HttpServer.h"
#include <boost/beast/core.hpp>
#include <thread>

namespace {
namespace net = boost::asio;
namespace http = boost::beast::http;
using tcp = net::ip::tcp;
using namespace yahat;

class Handler : public RequestHandler {
public:
    explicit Handler(std::function<Response(const Request&)> callback) : callback_{std::move(callback)} {}
    Response onReqest(const Request& request) override { return callback_(request); }
private:
    std::function<Response(const Request&)> callback_;
};

class HttpTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Reserve a candidate port; retrying client connects handles listener startup.
        net::io_context probe;
        tcp::acceptor acceptor{probe, {net::ip::make_address("127.0.0.1"), 0}};
        config_.http_endpoint = "127.0.0.1";
        config_.http_port = std::to_string(acceptor.local_endpoint().port());
        config_.num_http_threads = 2;
#ifdef YAHAT_ENABLE_METRICS
        config_.enable_metrics = false;
#endif
        server_ = std::make_unique<HttpServer>(config_, [](const AuthReq& auth_request) {
            Auth auth;
            auth.access = auth_request.auth_header == "Bearer good";
            return auth;
        });
    }

    void TearDown() override {
        if (started_) server_->stop();
    }

    void start() {
        server_->start();
        started_ = true;
    }

    http::response<http::string_body> exchange(http::request<http::string_body> request) {
        net::io_context ctx;
        boost::beast::tcp_stream stream{ctx};
        boost::system::error_code failure;
        http::response<http::string_body> response;
        request.version(11);
        request.set(http::field::host, "127.0.0.1");
        request.keep_alive(false);
        request.prepare_payload();
        net::spawn(ctx, [&](net::yield_context yield) {
            const tcp::endpoint endpoint{net::ip::make_address("127.0.0.1"),
                static_cast<unsigned short>(std::stoi(config_.http_port))};
            for (int attempt = 0; attempt < 100; ++attempt) {
                stream.expires_after(std::chrono::seconds{2});
                stream.async_connect(endpoint, yield[failure]);
                if (!failure) break;
                stream.socket().close();
                net::steady_timer pause{ctx, std::chrono::milliseconds{10}};
                pause.async_wait(yield);
            }
            if (failure) return;
            stream.expires_after(std::chrono::seconds{3});
            http::async_write(stream, request, yield[failure]);
            if (failure) return;
            boost::beast::flat_buffer buffer;
            http::async_read(stream, buffer, response, yield[failure]);
        }, net::detached);
        ctx.run();
        EXPECT_FALSE(failure) << failure.message();
        return response;
    }

    HttpConfig config_;
    std::unique_ptr<HttpServer> server_;
    bool started_ = false;
};

TEST_F(HttpTest, ParserBodyLimitReturnsPayloadTooLarge) {
    config_.max_request_body_size = 16;
    start();
    http::request<http::string_body> request{http::verb::post, "/mcp", 11};
    request.body() = std::string(32, 'x');
    EXPECT_EQ(exchange(request).result_int(), 413);
}

TEST_F(HttpTest, ParserHeaderLimitReturnsControlledFailure) {
    config_.max_request_header_size = 64;
    start();
    http::request<http::string_body> request{http::verb::post, "/mcp", 11};
    request.set("X-Large-Header", std::string(128, 'x'));
    EXPECT_EQ(exchange(request).result_int(), 431);
}

TEST_F(HttpTest, LegacyRoutesAndStatusBodiesRemainUnchanged) {
    server_->addRoute("/api", std::make_shared<Handler>([](const Request&) { return Response{}; }));
    start();
    http::request<http::string_body> request{http::verb::get, "/api/child", 11};
    request.set(http::field::authorization, "Bearer good");
    const auto response = exchange(request);
    EXPECT_EQ(response.result_int(), 200);
    EXPECT_EQ(response.body(), "{\"error\":false,\"status\":200,\"reason\":\"OK\"}");
    EXPECT_EQ(response[http::field::access_control_allow_origin], "*");
}

TEST_F(HttpTest, HeadersAreOwnedAndEmptyBodiesAreExplicit) {
    RouteOptions options;
    options.public_access = true;
    options.auto_handle_cors = false;
    server_->addRouteWithOptions("/mcp", std::make_shared<Handler>([](const Request& request) {
        EXPECT_EQ(request.getHeader("mCp-PrOtOcOl-VeRsIoN"), "test-version");
        auto copy = request.headers;
        EXPECT_EQ(copy["MCP-Protocol-Version"], "test-version");
        Response response{202, "Accepted"};
        response.generate_status_body = false;
        response.headers.set("Mcp-Session-Id", "session");
        response.headers.insert("X-Repeated", "one");
        response.headers.insert("X-Repeated", "two");
        return response;
    }), options);
    start();
    http::request<http::string_body> request{http::verb::post, "/mcp", 11};
    request.set("MCP-Protocol-Version", "test-version");
    const auto response = exchange(request);
    EXPECT_EQ(response.result_int(), 202);
    EXPECT_TRUE(response.body().empty());
    EXPECT_EQ(response["Mcp-Session-Id"], "session");
    EXPECT_EQ(response.count("X-Repeated"), 2);
    EXPECT_EQ(response.count(http::field::access_control_allow_origin), 0);
}

TEST_F(HttpTest, OriginValidationRunsBeforePreflightAndAuthentication) {
    RouteOptions options;
    options.allowed_origins = std::vector<std::string>{"https://trusted.example"};
    server_->addRouteWithOptions("/mcp", std::make_shared<Handler>([](const Request&) { return Response{}; }), options);
    start();
    http::request<http::string_body> request{http::verb::options, "/mcp", 11};
    request.set(http::field::origin, "https://untrusted.example");
    auto response = exchange(request);
    EXPECT_EQ(response.result_int(), 403);
    EXPECT_EQ(response.count(http::field::access_control_allow_origin), 0);
    request.set(http::field::origin, "https://trusted.example");
    response = exchange(request);
    EXPECT_EQ(response.result_int(), 200);
    EXPECT_EQ(response[http::field::access_control_allow_origin], "https://trusted.example");
    request.insert(http::field::origin, "https://trusted.example");
    EXPECT_EQ(exchange(request).result_int(), 403);
}

TEST_F(HttpTest, ExactRoutesAndCustomAuthenticationChallenge) {
    RouteOptions options;
    options.exact_match = true;
    options.auto_handle_cors = false;
    options.unauthorized_response = [](const Request&) {
        Response response{401, "Unauthorized"};
        response.headers.set(http::field::www_authenticate,
                             "Bearer resource_metadata=\"https://example.test/.well-known/oauth-protected-resource\"");
        return response;
    };
    server_->addRouteWithOptions("/mcp", std::make_shared<Handler>([](const Request&) { return Response{}; }), options);
    start();
    http::request<http::string_body> request{http::verb::get, "/mcp", 11};
    auto response = exchange(request);
    EXPECT_EQ(response.result_int(), 401);
    EXPECT_TRUE(response[http::field::www_authenticate].find("Bearer") == 0);
    request.set(http::field::authorization, "Bearer good");
    request.target("/mcp/child");
    EXPECT_EQ(exchange(request).result_int(), 404);
}

TEST_F(HttpTest, EndpointFilterAndAllowedMethods) {
    RouteOptions options;
    options.public_access = true;
    options.auto_handle_cors = false;
    options.allowed_methods = {Request::Type::POST};
    options.request_filter = [](const Request& request) -> std::optional<Response> {
        if (request.getHeader("X-Reject") == "yes") return Response{403, "Forbidden"};
        return {};
    };
    server_->addRouteWithOptions("/mcp", std::make_shared<Handler>([](const Request&) { return Response{}; }), options);
    start();
    http::request<http::string_body> request{http::verb::get, "/mcp", 11};
    auto response = exchange(request);
    EXPECT_EQ(response.result_int(), 405);
    EXPECT_EQ(response[http::field::allow], "POST");
    request.method(http::verb::options);
    request.set("X-Reject", "yes");
    EXPECT_EQ(exchange(request).result_int(), 403);
}

TEST_F(HttpTest, PostSseDrainsQueueAndTerminatesResponse) {
    RouteOptions options;
    options.public_access = true;
    auto sse = std::make_shared<SseQueueHandler>(*server_);
    sse->headers.set("X-Stream", "test");
    sse->sendSse("message", "first");
    sse->sendSse("message", "last");
    sse->finishSse();
    server_->addRouteWithOptions("/stream", std::make_shared<Handler>([sse](const Request&) {
        Response response;
        response.setContinuation(sse);
        return response;
    }), options);
    start();
    const auto response = exchange({http::verb::post, "/stream", 11});
    EXPECT_EQ(response[http::field::content_type], "text/event-stream");
    EXPECT_EQ(response["X-Stream"], "test");
    EXPECT_EQ(response.body(), "event: message\ndata: first\n\nevent: message\ndata: last\n\n");
    EXPECT_FALSE(sse->active());
}

TEST_F(HttpTest, IdleSseDisconnectNotifiesAndCancelsRequest) {
    RouteOptions options;
    options.public_access = true;
    auto sse = std::make_shared<SseQueueHandler>(*server_);
    auto notified = std::make_shared<std::promise<bool>>();
    auto completion = notified->get_future();
    server_->addRouteWithOptions("/stream", std::make_shared<Handler>([sse, notified](const Request& request) {
        const auto cancelled = request.cancelled;
        request.notify_connection_closed = [cancelled, notified] {
            notified->set_value(*cancelled);
        };
        Response response;
        response.setContinuation(sse);
        return response;
    }), options);
    start();
    net::io_context ctx;
    boost::beast::tcp_stream stream{ctx};
    boost::system::error_code failure;
    net::spawn(ctx, [&](net::yield_context yield) {
        const tcp::endpoint endpoint{net::ip::make_address("127.0.0.1"),
            static_cast<unsigned short>(std::stoi(config_.http_port))};
        for (int attempt = 0; attempt < 100; ++attempt) {
            stream.expires_after(std::chrono::seconds{2});
            stream.async_connect(endpoint, yield[failure]);
            if (!failure) break;
            stream.socket().close();
            net::steady_timer pause{ctx, std::chrono::milliseconds{10}};
            pause.async_wait(yield);
        }
        if (failure) return;
        http::request<http::empty_body> request{http::verb::post, "/stream", 11};
        request.set(http::field::host, "127.0.0.1");
        stream.expires_after(std::chrono::seconds{3});
        http::async_write(stream, request, yield[failure]);
        if (failure) return;
        boost::beast::flat_buffer buffer;
        http::response_parser<http::string_body> parser;
        http::async_read_header(stream, buffer, parser, yield[failure]);
        stream.socket().close();
    }, net::detached);
    ctx.run();
    ASSERT_FALSE(failure) << failure.message();
    ASSERT_EQ(completion.wait_for(std::chrono::seconds{3}), std::future_status::ready);
    EXPECT_TRUE(completion.get());
    EXPECT_FALSE(sse->active());
}

TEST_F(HttpTest, QueueLimitsAndClosedQueueAreSafe) {
    SseQueueHandler sse{*server_};
    sse.setQueueLimit(3);
    sse.sendSse("abc");
    EXPECT_THROW(sse.sendSse("d"), std::length_error);
    EXPECT_THROW(sse.setQueueLimit(2), std::length_error);
    EXPECT_THROW(sse.sendSse("bad\nevent", "data"), std::invalid_argument);
    sse.closeSse();
    EXPECT_NO_THROW(sse.sendSse("ignored"));
}

TEST(HttpHeadersTest, FramingAndHeaderInjectionAreRejected) {
    Response response;
    http::fields target;
    response.headers.set(http::field::content_length, "100");
    EXPECT_THROW(response.applyHeaders(target), std::invalid_argument);
    response.headers.clear();
    response.headers.set("X-Value", "safe\r\nInjected: unsafe");
    EXPECT_THROW(response.applyHeaders(target), std::invalid_argument);
}
} // namespace
