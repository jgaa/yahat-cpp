
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
    return boost::json::serialize(json_obj);
}

} // anon ns

ChatApi::ChatApi(ChatMgr &chatMgr)
    : chat_mgr_(chatMgr)
{
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
    if (auto cookie = req.getCookie("user"); !cookie.empty()) {
        user_uuid = boost::uuids::string_generator{}(cookie.begin(), cookie.end());
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
            resp.cookies.push_back(format("user={}", boost::uuids::to_string(uuid)));
            return resp;
        } catch (const std::exception &e) {
            return {400, e.what()};
        }
    };

    if (req.target == "/chat/stream") {
        string user;

        if (auto name = req.getArgument("user"); !name.empty()) {
            user = name;
            lock_guard lock{mutex_};
            for(const auto &[uuid, user] : users_) {
                if (user.name == name) {
                    user_uuid = uuid;
                    break;
                }
            }
            if (user_uuid.is_nil()) {
                return {400, "Unknown user"};
            }
        } else if (!user_uuid.is_nil()) {
            {
                lock_guard lock{mutex_};
                const auto it = users_.find(user_uuid);
                if (it == users_.end()) {
                    return {400, "Unknown user"};
                }
                user = it->second.name;
            }
        } else {
            return {400, "Missing 'user' cookie and argument"};
        }

        // Now, associate this user with this sse stream

        auto sse = make_shared<SseHandler>(chat_mgr_.server());
        {
            lock_guard lock{mutex_};
            auto &user = users_.at(user_uuid);
            user.sse_ = sse.get();
            user.sse_exists_ = sse->exists();
        }

        chat_mgr_.setEventCb(user, [user_uuid, this](auto event, auto user, auto message) {
            static constexpr auto event_names = to_array<string_view>({"message", "user-joined", "user-left"});

            SseHandler *sse{};
            {
                lock_guard lock{mutex_};
                if (auto it = users_.find(user_uuid); it != users_.end()) {
                    if (it->second.sse_exists_.expired()) {
                        LOG_DEBUG << "User " << user_uuid << " has disconnected, ignoring the sse stream";
                        return;
                    };
                    sse = it->second.sse_;
                    assert(sse);
                }
            }

            if (sse) {
                // Send the event it outside the lock
                sse->sendSse(format("event: {}\n{}\n",
                                    event_names.at(static_cast<uint>(event)),
                                    createJsonPayload(user, message)));
            }
        });

        return std::move(*sse);
    }

    return {400, "Unsupported request"};
}


ChatApi::SseHandler::~SseHandler()
{
    chat_mgr_->removeUser(user_uuid_);
}
