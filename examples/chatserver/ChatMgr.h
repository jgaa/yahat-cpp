#pragma once

#include <string>
#include <stdexcept>
#include <functional>
#include <map>
#include <mutex>
#include <memory>

#include <boost/asio.hpp>

class ChatMgr
{
public:
    ChatMgr(boost::asio::io_context& ioCtx)
        : io_ctx_{&ioCtx}
    {}

    enum class Event {
        MESSAGE,
        USER_JOINED,
        USER_LEFT
    };

    using event_callback_t = std::function<void(Event, std::string_view/*user*/, std::string_view/*message*/)>;

    struct User {
        event_callback_t callback;
    };

    struct already_exists : std::runtime_error {
        already_exists() : std::runtime_error("User already exists") {}
    };

    /*! Add a user to the chat
     * \param name The name of the user
     * \throw std::invalid_argument if the name is empty
     * \throw already_exists if the user already exists
     */
    void addUser(const std::string& name);

    void setEventCb(std::string_view name, event_callback_t && cb);

    /*! Remove a user from the chat
     * \param handle The handle of the user
     */
    void removeUser(std::string name) noexcept;
    //void sendMessage(handle_t from, handle_t to, const std::string& message);

    /*! Send a message to the chat
     */
    void sendMessage(std::string name, const std::string& message) noexcept;

    std::vector<std::string> listUsers();

    auto& ioCtx() { return *io_ctx_; }

private:
    void sendEvent(Event event, std::string_view user, std::string_view message = {});
    // Wrap mutex in a padding to make sure it's on its own cache line
    std::mutex mutex_;
    std::map<std::string, std::shared_ptr<User>> users_;
    boost::asio::io_context *io_ctx_{};
};
