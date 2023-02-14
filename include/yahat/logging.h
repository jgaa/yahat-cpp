#pragma once

#include "yahat/config.h"

#ifdef USE_LOGFAULT
#include "logfault/logfault.h"

#define LOG_ERROR   LFLOG_ERROR
#define LOG_WARN    LFLOG_WARN
#define LOG_INFO    LFLOG_INFO
#define LOG_DEBUG   LFLOG_DEBUG
#define LOG_TRACE   LFLOG_TRACE

#else

#include <iostream>
#include <sstream>
#include <functional>
#include <cassert>

namespace yahat {
enum class LogLevel {
    MUTED, // Don't log anything at this level
    LERROR,  // Thank you Microsoft, for polluting the global namespace with all your crap
    WARNING,
    INFO,
    DEBUG,
    TRACE
};

class LogEvent {
public:
    LogEvent(LogLevel level)
        : level_{level} {}

    ~LogEvent();

    std::ostringstream& Event() { return msg_; }
    LogLevel Level() const noexcept { return level_; }

private:
    const LogLevel level_;
    std::ostringstream msg_;
};

/*! Internal logger class
 *
 *  This is a verty simple internal log handler that is designd to
 *  forward log events to whatever log framework you use in your
 *  application.
 *
 *  You must set a handler before you call any methods in restc,
 *  and once set, the handler cannot be changed. This is simply
 *  to avoid a memory barrier each time the library create a
 *  log event, just to access the log handler.
 *
 *  You can change the log level at any time.
 */
class Logger {
    public:

    using log_handler_t = std::function<void(LogLevel level, const std::string& msg)>;

    Logger() = default;

    static Logger& Instance() noexcept;

    LogLevel GetLogLevel() const noexcept {return current_; }
    void SetLogLevel(LogLevel level) { current_ = level; }

    /*! Set a log handler.
     *
     *  This can only be done once, and should be done when the library are being
     *  initialized, before any other library methods are called.
     */
    void SetHandler(log_handler_t handler) {
        assert(!handler_);
        handler_ = handler;
    }

    bool Relevant(LogLevel level) const noexcept {
        return handler_ && level <= current_;
    }

    void onEvent(LogLevel level, const std::string& msg) {
        Instance().handler_(level, msg);
    }

private:
    log_handler_t handler_;
    LogLevel current_ = LogLevel::INFO;
};
} // ns

#define LOG_EVENT_(level) yahat::Logger::Instance().Relevant(level) && yahat::LogEvent{level}.Event()

#define LOG_ERROR   LOG_EVENT_(yahat::LogLevel::LERROR)
#define LOG_WARN    LOG_EVENT_(yahat::LogLevel::WARNING)
#define LOG_INFO    LOG_EVENT_(yahat::LogLevel::INFO)
#define LOG_DEBUG   LOG_EVENT_(yahat::LogLevel::DEBUG)
#define LOG_TRACE   LOG_EVENT_(yahat::LogLevel::TRACE)

#endif

