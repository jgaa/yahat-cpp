#include <gtest/gtest.h>
#include "yahat/JsonRpc.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#ifdef YAHAT_ENABLE_JSON_RPC
namespace {
using namespace yahat;
namespace json = boost::json;

int errorCode(const std::optional<json::value>& response) {
    return static_cast<int>(response->as_object().at("error").as_object().at("code").as_int64());
}

TEST(JsonRpcTest, EchoesIdsAndPassesRequestContext) {
    JsonRpc rpc;
    Request request;
    request.auth.account = "alice";
    rpc.addMethod("echo", [](const json::value& params, const JsonRpc::Context& context) {
        EXPECT_EQ(context.request->auth.account, "alice");
        EXPECT_FALSE(context.notification);
        return params;
    });
    for (const auto* id : {"1", "\"abc\"", "null", "1.5"}) {
        auto call = json::parse(R"({"jsonrpc":"2.0","method":"echo","params":{"a":1}})");
        call.as_object()["id"] = json::parse(id);
        const auto response = rpc.dispatch(call, &request);
        ASSERT_TRUE(response);
        EXPECT_EQ(response->as_object().at("id"), json::parse(id));
        EXPECT_EQ(response->as_object().at("result"), call.as_object().at("params"));
        EXPECT_FALSE(response->as_object().contains("error"));
    }
}

TEST(JsonRpcTest, NotificationsNeverProduceResponses) {
    JsonRpc rpc;
    int invoked = 0;
    rpc.addMethod("notify", [&](const json::value& params, const JsonRpc::Context& context) -> json::value {
        EXPECT_TRUE(params.is_null());
        EXPECT_TRUE(context.notification);
        ++invoked;
        throw JsonRpc::Error{JsonRpc::invalid_params, "Bad arguments"};
    });
    EXPECT_FALSE(rpc.handle(R"({"jsonrpc":"2.0","method":"notify"})"));
    EXPECT_FALSE(rpc.handle(R"({"jsonrpc":"2.0","method":"unknown"})"));
    EXPECT_EQ(invoked, 1);
}

TEST(JsonRpcTest, ValidatesEnvelopesAndReturnsStandardErrors) {
    JsonRpc rpc;
    EXPECT_EQ(errorCode(rpc.handle("{")), JsonRpc::parse_error);
    EXPECT_EQ(errorCode(rpc.handle(R"({"jsonrpc":"2.0","method":"missing","id":1})")), JsonRpc::method_not_found);
    for (const auto* invalid : {"null", "[]", "1", R"({"method":"missing"})",
        R"({"jsonrpc":"1.0","method":"missing","id":2})",
        R"({"jsonrpc":"2.0","method":5})",
        R"({"jsonrpc":"2.0","method":"missing","id":true})",
        R"({"jsonrpc":"2.0","method":"missing","id":[]})",
        R"({"jsonrpc":"2.0","method":"missing","params":null})",
        R"({"jsonrpc":"2.0","method":"missing","params":1})"}) {
        const auto response = rpc.handle(invalid);
        EXPECT_EQ(errorCode(response), JsonRpc::invalid_request) << invalid;
        EXPECT_TRUE(response->as_object().at("id").is_null());
    }
}

TEST(JsonRpcTest, HandlesBatchesWithoutReplyingToNotifications) {
    JsonRpc rpc;
    rpc.addMethod("echo", [](const json::value& params, const JsonRpc::Context&) { return params; });
    const auto response = rpc.handle(R"([
        {"jsonrpc":"2.0","method":"echo","params":[1],"id":1},
        {"jsonrpc":"2.0","method":"echo"},
        5,
        {"jsonrpc":"2.0","method":"missing","id":2}
    ])");
    ASSERT_TRUE(response);
    const auto& batch = response->as_array();
    ASSERT_EQ(batch.size(), 3);
    EXPECT_EQ(batch[0].as_object().at("result"), json::parse("[1]"));
    EXPECT_TRUE(batch[1].as_object().at("id").is_null());
    EXPECT_EQ(batch[2].as_object().at("id"), json::value{2});
    EXPECT_FALSE(rpc.handle(R"([{"jsonrpc":"2.0","method":"echo"}])"));
    const auto nested = rpc.handle("[[1]]");
    EXPECT_EQ(nested->as_array()[0].as_object().at("error").as_object().at("code").as_int64(), JsonRpc::invalid_request);
}

TEST(JsonRpcTest, BatchLimitsCanEnforceSingleMessageTransports) {
    JsonRpc rpc{JsonRpc::Options{false, 10}};
    EXPECT_EQ(errorCode(rpc.handle(R"([{"jsonrpc":"2.0","method":"x","id":1}])")), JsonRpc::invalid_request);
    JsonRpc bounded{JsonRpc::Options{true, 1}};
    EXPECT_EQ(errorCode(bounded.handle("[1,2]")), JsonRpc::invalid_request);
}

TEST(JsonRpcTest, ProtocolErrorsAreExplicitAndUnexpectedErrorsArePrivate) {
    JsonRpc rpc;
    rpc.addMethod("invalid", [](const json::value&, const JsonRpc::Context&) -> json::value {
        throw JsonRpc::Error{JsonRpc::invalid_params, "Missing name", json::object{{"field", "name"}}};
    });
    rpc.addMethod("broken", [](const json::value&, const JsonRpc::Context&) -> json::value {
        throw std::runtime_error{"private credentials must not escape"};
    });
    const auto response = rpc.handle(R"({"jsonrpc":"2.0","method":"invalid","id":9})");
    EXPECT_EQ(errorCode(response), JsonRpc::invalid_params);
    EXPECT_EQ(response->as_object().at("error").as_object().at("data"), json::parse(R"({"field":"name"})"));
    const auto broken = rpc.handle(R"({"jsonrpc":"2.0","method":"broken","id":9})");
    EXPECT_EQ(errorCode(broken), JsonRpc::internal_error);
    EXPECT_EQ(broken->as_object().at("error").as_object().at("message"), json::value{"Internal error"});
}

TEST(JsonRpcTest, CallbacksCanModifyTheRegistryWithoutDeadlock) {
    JsonRpc rpc;
    rpc.addMethod("remove", [&](const json::value&, const JsonRpc::Context&) -> json::value {
        return rpc.removeMethod("remove");
    });
    EXPECT_TRUE(rpc.handle(R"({"jsonrpc":"2.0","method":"remove","id":1})")->as_object().at("result").as_bool());
    EXPECT_EQ(errorCode(rpc.handle(R"({"jsonrpc":"2.0","method":"remove","id":2})")), JsonRpc::method_not_found);
    EXPECT_THROW(rpc.addMethod("rpc.reserved", [](const json::value&, const JsonRpc::Context&) { return nullptr; }), std::invalid_argument);
}

TEST(JsonRpcTest, NativeAsyncDispatchOwnsContextAndPreservesNotificationsAndErrors) {
    namespace net = boost::asio;
    JsonRpc rpc;
    auto request = std::make_shared<Request>();
    request->auth.account = "alice";
    rpc.addAsyncMethod("wait", [](json::value params, JsonRpc::Context context) -> net::awaitable<json::value> {
        net::steady_timer timer{co_await net::this_coro::executor, std::chrono::milliseconds{1}};
        co_await timer.async_wait(net::use_awaitable);
        EXPECT_EQ(context.request->auth.account, "alice");
        co_return params;
    });
    rpc.addAsyncMethod("fail", [](json::value, JsonRpc::Context) -> net::awaitable<json::value> {
        throw std::runtime_error{"secret"}; co_return nullptr;
    });
    rpc.addMethod("sync", [](const json::value&, const JsonRpc::Context&) { return true; });
    auto batch = json::parse(R"([
        {"jsonrpc":"2.0","id":"a","method":"wait","params":{"x":1}},
        {"jsonrpc":"2.0","method":"wait"},
        {"jsonrpc":"2.0","id":2,"method":"sync"},
        {"jsonrpc":"2.0","id":3,"method":"fail"}
    ])");
    net::io_context ctx;
    auto future = net::co_spawn(ctx, rpc.dispatchAsync(std::move(batch), request), net::use_future);
    request.reset();
    ctx.run();
    const auto reply = future.get();
    ASSERT_TRUE(reply);
    ASSERT_EQ(reply->as_array().size(), 3);
    EXPECT_EQ(reply->as_array()[0].as_object().at("id"), "a");
    EXPECT_EQ(reply->as_array()[0].as_object().at("result").as_object().at("x"), 1);
    EXPECT_EQ(reply->as_array()[1].as_object().at("result"), true);
    EXPECT_EQ(reply->as_array()[2].as_object().at("error").as_object().at("message"), "Internal error");
    EXPECT_THROW(rpc.addMethod("wait", [](const json::value&, const JsonRpc::Context&) { return nullptr; }), std::invalid_argument);
    EXPECT_TRUE(rpc.removeMethod("wait"));
}

TEST(JsonRpcTest, HttpAdapterUsesPostJsonAndEmptyAcceptedResponses) {
    auto rpc = std::make_shared<JsonRpc>();
    rpc->addMethod("ping", [](const json::value&, const JsonRpc::Context&) { return json::object{}; });
    JsonRpcHandler handler{rpc};
    Request request;
    EXPECT_EQ(handler.onReqest(request).code, 405);
    request.type = Request::Type::POST;
    EXPECT_EQ(handler.onReqest(request).code, 415);
    request.headers.set("Content-Type", "Application/JSON; charset=utf-8");
    request.body = R"({"jsonrpc":"2.0","method":"ping"})";
    auto response = handler.onReqest(request);
    EXPECT_EQ(response.code, 202);
    EXPECT_FALSE(response.generate_status_body);
    EXPECT_TRUE(response.body.empty());
    request.body = R"({"jsonrpc":"2.0","method":"ping","id":1})";
    response = handler.onReqest(request);
    EXPECT_EQ(response.code, 200);
    EXPECT_EQ(response.mime_type, "application/json");
    EXPECT_TRUE(json::parse(response.body).as_object().contains("result"));
}
} // namespace
#endif
