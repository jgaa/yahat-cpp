#pragma once

#include "yahat/HttpServer.h"
#include "yahat/logging.h"

class WebApp : public yahat::RequestHandler
{
public:
    WebApp();

    // RequestHandler interface
    yahat::Response onReqest(const yahat::Request &req) override;
};
