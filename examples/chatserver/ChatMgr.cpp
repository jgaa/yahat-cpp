#include "ChatMgr.h"

ChatMgr::ChatMgr() {}

using namespace std;

void ChatMgr::addUser(const std::string &name)
{
    if (name.empty())
        throw std::invalid_argument("Name cannot be empty");

    {
        lock_guard lock{mutex_};
        if (users_.find(name) != users_.end())
            throw already_exists();

        users_.emplace(name, make_shared<User>());
    }

    sendEvent(Event::USER_JOINED, name);
}

void ChatMgr::setEventCb(std::string_view name, event_callback_t && cb)
{
    lock_guard lock{mutex_};
    auto it = users_.find(string{name});
    if (it == users_.end()) {
        throw std::invalid_argument("User not found");
    }
    it->second->callback = std::move(cb);
}

void ChatMgr::removeUser(std::string name) noexcept
{
    {
        lock_guard lock{mutex_};
        users_.erase(name);
    }
    sendEvent(Event::USER_LEFT, name);
}

void ChatMgr::sendMessage(std::string name, const std::string &message) noexcept
{
    sendEvent(Event::MESSAGE, name + ": " + message);
}

void ChatMgr::sendEvent(Event event, std::string_view username, std::string_view message)
{
    // Don't send the messages while we hold the lock

    vector<weak_ptr<User>> tmp;

    {
        lock_guard lock{mutex_};
        tmp.reserve(users_.size());
        for (const auto &[_, user] : users_) {
            tmp.push_back(user);
        }
    }

    for (const auto &wuser : tmp) {
        if (auto user = wuser.lock()) {
            if (user->callback) {
                user->callback(event, username, message);
            }
        }
    }
}

std::vector<string> ChatMgr::listUsers()
{
    vector<string> users;
    lock_guard lock{mutex_};
    for (const auto &[name, _] : users_) {
        users.push_back(name);
    }
    return users;
}
