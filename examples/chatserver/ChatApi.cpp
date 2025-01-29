
#include <format>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/json.hpp>
#include "yahat/logging.h"
#include "ChatApi.h"
#include "ChatMgr.h"

using namespace std;
using namespace yahat;

namespace {
std::string createJsonPayload(std::string_view username, std::string_view message) {
    // Create a JSON object
    boost::json::object json_obj;
    json_obj["username"] = username;

    if (!message.empty()) {
        json_obj["message"] = message;
    }

    // Convert the JSON object to a string and return it
    string json = boost::json::serialize(json_obj);
    return json;
}

} // anon ns

ChatApi::ChatApi(ChatMgr &chatMgr)
    : chat_mgr_(chatMgr)
{
    chat_mgr_.setDeletedUserNotificationCb([this](auto name) {
        string user_name{name};

        // Postpone the removal of the user to another stack-frame so we don't
        // get into interesting problems with the locks here and in the chat manager.
        boost::asio::post(chat_mgr_.server().getCtx(), [this, user_name] () {
            lock_guard lock{mutex_};
            for(const auto &user : users_) {
                if (user.second.name == user_name) {
                    // Just remove it. This is a call-back form the chat manager, so the user is already gone.
                    users_.erase(user.first);
                    break;
                }
            };
        });
    });

    enableMetrics(chatMgr.server(), "/chat");
}

yahat::Response ChatApi::onReqest(const yahat::Request &req)
{
    LOG_DEBUG << "ChatApi: Processing request " << req.uuid << " for target " << req.target;

    optional<decltype(boost::json::parse(""))> json;
    if (!req.body.empty()) {
        try {
            json = boost::json::parse(req.body);
        } catch (const exception &e) {
            return {400, format("Invalid JSON payload: ", e.what())};
        }
    }

    boost::uuids::uuid user_uuid;
    string user_name;
    if (auto cookie = req.getCookie("user"); !cookie.empty()) {
        user_uuid = boost::uuids::string_generator{}(cookie.begin(), cookie.end());
        user_name = getUser(user_uuid, false);
    };

    if (req.type == Request::Type::POST && req.target == "/chat/join") {

        if (!json) {
            return {400, "Excpected JSON payload with the name!"};
        }

        const std::string user{json->at("username").as_string()};

        try {
            chat_mgr_.addUser(user);
            auto uuid = boost::uuids::random_generator()();
            addUser(uuid, user);
            auto resp = Response{200, "OK"};
            resp.cookies.emplace_back("user",
                                      format("{}; Path=/;{} HttpOnly; SameSite=Strict",
                                             boost::uuids::to_string(uuid),
                                             (req.isHttps() ? " Secure;" : "")));
            return resp;
        } catch (const std::exception &e) {
            return {400, e.what()};
        }
    } else if (req.type == Request::Type::POST && req.target == "/chat/message") {
        if (!json) {
            return {400, "Excpected JSON payload with the message!"};
        }

        if (auto message = json->at("message").as_string(); !message.empty()) {
            if (!user_name.empty()) {
                chat_mgr_.sendMessage(user_name, message);
                return {200, "OK"};
            } else {
                return {400, "User not found"};
            }
        } else {
            return {400, "Empty message"};
        }
    } else if (req.type == Request::Type::GET && req.target == "/chat/stream") {
        auto sse = make_shared<SseHandler>(chat_mgr_.server());
        {
            lock_guard lock{mutex_};
            LOG_DEBUG << "Have the lock...";
            auto it = users_.find(user_uuid);
            if (it == users_.end()) {
                return {400, "User not found"};
            }
            auto &user = it->second;
            user.name = user_name;
            user.sse_ = sse;
        }

        // Empty message to just send the header to conform the request.
        sse->sendSse({});
        std::weak_ptr<SseHandler> wsse = sse;

        chat_mgr_.setEventCb(user_name, [user_uuid, this](auto event, auto user, auto message) {
            static constexpr auto event_names = to_array<string_view>({"message", "user-joined", "user-left"});

            if (auto sse = getSse(user_uuid)) {
                const auto data = createJsonPayload(user, message);
                const auto event_name = event_names.at(static_cast<uint>(event));
                sse->sendSse(event_name, data);
            }
        }, [wsse]() -> bool {
            return !wsse.expired();
        });

        Response response{200, "OK"};
        response.setContinuation(std::move(sse));
#ifdef YAHAT_ENABLE_METRICS
        if (req.requestDuration) {
            // We don't want long-running SSE requests to affect the metrics for normal requests.
            req.requestDuration->cancel();
        }
#endif
        return response;
    } else if (req.type == Request::Type::GET && req.target == "/chat/users") {
        auto users = chat_mgr_.listUsers();
        // Use boost::json to create the users and export it to json
        boost::json::array json_users;
        for (const auto &user : users) {
            json_users.emplace_back(user);
        }
        const auto json = boost::json::serialize(json_users);
        return {200, "OK", json};
    } else if (req.type == Request::Type::POST && req.target == "/chat/logout") {
        chat_mgr_.removeUser(user_name);
        removeUser(user_uuid);
    }

    return {400, "Unsupported request"};
}

string ChatApi::getUser(const boost::uuids::uuid &uuid, bool throwOnNotFound)
{
    lock_guard lock{mutex_};
    const auto it = users_.find(uuid);

    if (it != users_.end()) {
        return it->second.name;
    }

    if (throwOnNotFound) {
        throw invalid_argument("User not found");
    }

    return {};
}

