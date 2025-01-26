#pragma once

#include <mutex>
#include <map>

#include "yahat/HttpServer.h"

class ChatMgr;

class ChatApi : public yahat::RequestHandler
{
public:
    class SseHandler : public yahat::SseQueueHandler {
    public:
        SseHandler(yahat::HttpServer& server)
            : SseQueueHandler(server)
        {}

        ~SseHandler();
    };

    struct User {
        std::string name;
        std::weak_ptr<SseHandler> sse_;
    };

    ChatApi(ChatMgr& chatMgr);

    // RequestHandler interface
    yahat::Response onReqest(const yahat::Request &req) override;

private:
    void addUser(const boost::uuids::uuid& uuid, const std::string& name) {
        std::lock_guard lock{mutex_};
        if (users_.find(uuid) != users_.end())
            throw std::invalid_argument("User already exists");
        users_.emplace(uuid, name);
    }

    void removeUser(const boost::uuids::uuid& uuid) {
        std::lock_guard lock{mutex_};
        users_.erase(uuid);
    }

    // void setSseReq(boost::uuids::uuid uuid, const yahat::Request& req) {
    //     std::lock_guard lock{mutex_};
    //     if (auto it = users_.find(uuid); it != users_.end()) {
    //         it->second.sse_req = const_cast<yahat::Request&>(req).weak_from_this();
    //     } else {
    //         throw std::invalid_argument("User dont exist");
    //     }
    // }

    ChatMgr& chat_mgr_;
    std::map<boost::uuids::uuid, User> users_;
    std::mutex mutex_;
};
