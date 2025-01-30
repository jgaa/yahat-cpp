/**
 * @file ChatApi.h
 * @brief Declares the ChatApi class responsible for handling HTTP requests for a chat server.
 */

#pragma once

#include <mutex>
#include <map>

#include "yahat/HttpServer.h"
#include "yahat/Metrics.h"

class ChatMgr;

/**
 * @class ChatApi
 * @brief Handles HTTP requests related to chat operations and manages user connections.
 */
class ChatApi : public yahat::RequestHandler {
public:
    /**
     * @class SseHandler
     * @brief Handles server-sent events (SSE) for individual users.
     */
    class SseHandler : public yahat::SseQueueHandler {
    public:
        /**
         * @brief Constructs an SseHandler object.
         *
         * @param server Reference to the HTTP server instance.
         */
        SseHandler(yahat::HttpServer& server)
            : SseQueueHandler(server)
        {}

        /**
         * @brief Destructor for SseHandler.
         *
         * Performs cleanup if necessary.
         */
        ~SseHandler() = default;
    };

    /**
     * @struct User
     * @brief Represents a user connected to the chat system.
     */
    struct User {
        explicit User(const std::string& name) : name(name) {}
        User() = default;

        std::string name; ///< The name of the user.
        std::weak_ptr<SseHandler> sse_; ///< Weak pointer to the user's SSE handler.
    };

    /**
     * @brief Constructs a ChatApi instance.
     *
     * @param chatMgr Reference to the ChatMgr instance that manages chat rooms.
     */
    ChatApi(ChatMgr& chatMgr);

    /**
     * @brief Handles an incoming HTTP request.
     *
     * @param req The HTTP request to process.
     * @return A response object containing the result of the request handling.
     */
    yahat::Response onReqest(const yahat::Request &req) override;

private:
    /**
     * @brief Adds a new user to the system.
     *
     * @param uuid The UUID of the user.
     * @param name The name of the user.
     * @throws std::invalid_argument if the user already exists.
     */
    void addUser(const boost::uuids::uuid& uuid, const std::string& name) {
        std::lock_guard lock{mutex_};
        if (users_.find(uuid) != users_.end())
            throw std::invalid_argument("User already exists");
        users_.emplace(uuid, name);
    }

    /**
     * @brief Removes a user from the system.
     *
     * @param uuid The UUID of the user to remove.
     */
    void removeUser(const boost::uuids::uuid& uuid) {
        std::lock_guard lock{mutex_};
        users_.erase(uuid);
    }

    /**
     * @brief Retrieves the SSE handler for a user by UUID.
     *
     * @param uuid The UUID of the user.
     * @return A shared pointer to the user's SSE handler.
     * @throws std::invalid_argument if the user is not found.
     */
    auto getSse(const boost::uuids::uuid& uuid) {
        std::lock_guard lock{mutex_};
        auto it = users_.find(uuid);
        if (it == users_.end())
            throw std::invalid_argument("User not found");
        return it->second.sse_.lock();
    }

    /**
     * @brief Retrieves the name of a user by UUID.
     *
     * @param uuid The UUID of the user.
     * @param throwOnNotFound Whether to throw an exception if the user is not found.
     * @return The name of the user.
     */
    std::string getUser(const boost::uuids::uuid& uuid, bool throwOnNotFound);

    ChatMgr& chat_mgr_; ///< Reference to the ChatMgr instance for managing chat rooms.
    std::map<boost::uuids::uuid, User> users_; ///< Map of users indexed by their UUIDs.
#ifdef YAHAT_ENABLE_METRICS
    yahat::Metrics::Summary<double> *req_duration_metric_{}; ///< Pointer to the request duration metric.
#endif
    alignas(64u) std::mutex mutex_; ///< Mutex to ensure thread-safe access to the users map.
};
