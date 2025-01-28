/**
 * @file WebApp.h
 * @brief Declares the WebApp class that provides the HTML/JavaScript code for web browsers.
 */

#pragma once

#include "yahat/HttpServer.h"
#include "yahat/logging.h"

/**
 * @class WebApp
 * @brief Handles HTTP requests to serve HTML and JavaScript content for the web-based client.
 */
class WebApp : public yahat::RequestHandler {
public:
    /**
     * @brief Constructs a WebApp instance.
     *
     * Initializes the request handler for serving web application content.
     */
    WebApp();

    /**
     * @brief Handles an incoming HTTP request.
     *
     * Processes requests for web application resources (e.g., HTML, JavaScript, CSS) and returns
     * the appropriate response.
     *
     * @param req The incoming HTTP request.
     * @return A response object containing the requested content.
     */
    yahat::Response onReqest(const yahat::Request &req) override;
};
