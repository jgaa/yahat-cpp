/**
 * @file ChatMgr.h
 * @brief Declares the ChatMgr class for managing users and events in a chat system.
 */

#pragma once

#include <string>
#include <stdexcept>
#include <functional>
#include <map>
#include <mutex>
#include <memory>

#include <boost/asio.hpp>

#include "yahat/HttpServer.h"

/**
 * @class ChatMgr
 * @brief Manages chat users, events, and communication within the chat system.
 */
class ChatMgr {
public:
    /**
     * @brief Type alias for a probe function to check if a user is still active.
     */
    using probe_t = std::function<bool()>;

    /**
     * @brief Type alias for a callback function to notify when a user is deleted.
     */
    using deleted_cb_t = std::function<void(std::string_view)>;

    /**
     * @brief Constructs a ChatMgr instance.
     *
     * @param server Reference to the HTTP server instance.
     */
    ChatMgr(yahat::HttpServer& server);

    /**
     * @enum Event
     * @brief Defines chat-related events.
     */
    enum class Event {
        MESSAGE,      ///< A message event.
        USER_JOINED,  ///< A user joined event.
        USER_LEFT     ///< A user left event.
    };

    /**
     * @brief Type alias for an event callback function.
     *
     * This function is called when an event occurs and takes the event type,
     * the user involved, and an optional message.
     */
    using event_callback_t = std::function<void(Event, std::string_view /*user*/, std::string_view /*message*/)>;

    /**
     * @struct User
     * @brief Represents a user in the chat system.
     */
    struct User {
        User() = default;

        /**
         * @brief Constructs a User object with a liveness probe.
         *
         * @param isAlive A probe function to check if the user is still active.
         */
        User(probe_t isAlive);

        event_callback_t callback; ///< Callback function for the user.
        const std::chrono::steady_clock::time_point created{std::chrono::steady_clock::now()}; ///< Creation time of the user.
        probe_t is_alive; ///< Liveness probe function.
    };

    /**
     * @class already_exists
     * @brief Exception indicating a user with the same name already exists.
     */
    struct already_exists : std::runtime_error {
        already_exists() : std::runtime_error("User already exists") {}
    };

    /**
     * @brief Adds a user to the chat.
     *
     * @param name The name of the user.
     * @throws std::invalid_argument if the name is empty.
     * @throws already_exists if the user already exists.
     */
    void addUser(const std::string& name);

    /**
     * @brief Sets an event callback for a user.
     *
     * The callback is used by the chat manager when sending an
     * event to all the users in the chat.
     *
     * @param name The name of the user.
     * @param cb The event callback function.
     * @param isAlive The liveness probe function for the user.
     */
    void setEventCb(std::string_view name, event_callback_t&& cb, probe_t isAlive);

    /**
     * @brief Removes a user from the chat.
     *
     * @param name The name of the user.
     */
    void removeUser(std::string name) noexcept;

    /**
     * @brief Sends a message to the chat.
     *
     * @param name The name of the sender.
     * @param message The message content.
     */
    void sendMessage(std::string name, std::string_view message) noexcept;

    /**
     * @brief Lists all active users in the chat.
     *
     * @return A vector of user names.
     */
    std::vector<std::string> listUsers();

    /**
     * @brief Gets the I/O context of the server.
     *
     * @return Reference to the boost::asio::io_context object.
     */
    auto& ioCtx() { return server_.getCtx(); }

    /**
     * @brief Gets the HTTP server instance.
     *
     * @return Reference to the HTTP server.
     */
    auto& server() {
        return server_;
    }

    /**
     * @brief Sets the callback function for deleted user notifications.
     *
     * @param cb The callback function.
     * @note Only one callback can be set.
     */
    void setDeletedUserNotificationCb(deleted_cb_t cb) {
        assert(!deleted_cb_ && "Only one callback can be set");
        deleted_cb_ = std::move(cb);
    }

private:
    /**
     * @brief Sends an event notification.
     *
     * @param event The type of event.
     * @param user The name of the user involved.
     * @param message An optional message associated with the event.
     */
    void sendEvent(Event event, std::string_view user, std::string_view message = {});

    /**
     * @brief Performs housekeeping tasks for the chat system.
     */
    void housekeeping();

    /**
     * @brief Starts the housekeeping timer.
     */
    void startHousekeepingTimer();

    std::mutex mutex_; ///< Mutex for thread-safe access to the users map.
    std::map<std::string, std::shared_ptr<User>> users_; ///< Map of users indexed by their names.
    yahat::HttpServer& server_; ///< Reference to the HTTP server instance.
    boost::asio::steady_timer timer_{server_.getCtx()}; ///< Timer for housekeeping tasks.
    deleted_cb_t deleted_cb_; ///< Callback for deleted user notifications.
};
