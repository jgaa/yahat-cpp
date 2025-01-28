#include "ChatMgr.h"

#include "yahat/logging.h"

using namespace std;

ChatMgr::ChatMgr(yahat::HttpServer &server)
    : server_{server}
{
    startHousekeepingTimer();
}

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

void ChatMgr::setEventCb(std::string_view name, event_callback_t && cb, probe_t isAlive)
{
    assert(isAlive);
    assert(cb);

    lock_guard lock{mutex_};
    auto it = users_.find(string{name});
    if (it == users_.end()) {
        throw std::invalid_argument("User not found");
    }
    it->second->callback = std::move(cb);
    it->second->is_alive = std::move(isAlive);
}

void ChatMgr::removeUser(std::string name) noexcept
{
    LOG_DEBUG << "Removing user " << name;
    sendEvent(Event::USER_LEFT, name);
    {
        lock_guard lock{mutex_};
        users_.erase(name);
    }

    if (deleted_cb_) {
        deleted_cb_(name);
    }
}

void ChatMgr::sendMessage(std::string name, const std::string_view message) noexcept
{
    sendEvent(Event::MESSAGE, name, message);
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

void ChatMgr::housekeeping()
{
    LOG_TRACE << "Housekeeping";
    vector<string> users_to_delete;
    {
        {
            // Just copy the names to be deleted under the lock.
            lock_guard lock{mutex_};
            for (const auto &[name, user] : users_) {
                if (user->is_alive) { // Has the callback set, which mean that a sse channel was created
                    if (!user->is_alive()) { // Still has a sse channel?
                        LOG_DEBUG << "User " << name << " is not alive";
                        users_to_delete.push_back(name);
                    }
                } else if (chrono::steady_clock::now() - user->created > 10s) {
                    LOG_DEBUG << "User " << name << " has not created it's SSE connection in time";
                    users_to_delete.push_back(name);
                }
            }
        }

        for (const auto &name : users_to_delete) {
            removeUser(name);
        }
    }
}

void ChatMgr::startHousekeepingTimer()
{
    timer_.expires_after(10s);
    timer_.async_wait([this](const auto ec) {
        if (!ec) {
            housekeeping();
            startHousekeepingTimer();
        } else {
            LOG_ERROR << "Timer error: " << ec.message();
        }
    });
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
