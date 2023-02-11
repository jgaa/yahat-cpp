# include "yahat/logging.h"

#ifndef USE_LOGFAULT

using namespace std;

namespace yahat {

Logger &Logger::Instance() noexcept
{
    static Logger logger;
    return logger;
}

LogEvent::~LogEvent()
{
    Logger::Instance().onEvent(level_, msg_.str());
}

}



#endif
