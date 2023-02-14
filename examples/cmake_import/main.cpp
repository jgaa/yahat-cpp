
#include <iostream>
#include <filesystem>
#include <boost/program_options.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "yahat/HttpServer.h"
#include "yahat/logging.h"

using namespace std;
using namespace yahat;

HttpConfig config;
std::string log_level = "info";

namespace po = boost::program_options;
po::options_description general("Options");
po::positional_options_description positionalDescription;

class MyRequestHandler : public RequestHandler {
    // RequestHandler interface
public:
    Response onReqest(const Request &req, const Auth &auth) override {
        LOG_DEBUG << "MyRequestHandler: Processing request "
        << req.uuid
        << " for account "
        <<  auth.account;

        if (req.type != Request::Type::GET) {
            return {405, "Method Not Allowed - only GET is allowed here"};
        }

        return {};
    }
};

int main(int argc, char* argv[]) {
    HttpConfig config;
    std::string log_level = "info";

    namespace po = boost::program_options;
    po::options_description general("Options");
    po::positional_options_description positionalDescription;

    general.add_options()
        ("help,h", "Print help and exit")
        ("version", "print version string and exit")
        ("log-level,l",
             po::value<string>(&log_level)->default_value(log_level),
             "Log-level to use; one of 'info', 'debug', 'trace'")
    ;
    po::options_description http("HTTP/API server");
    http.add_options()
        ("http-endpoint,H",
            po::value<string>(&config.http_endpoint)->default_value(config.http_endpoint),
            "HTTP endpoint. For example [::] to listen to all interfaces")
        ("http-port",
            po::value<string>(&config.http_port)->default_value(config.http_port),
            "HTTP port to listen to. Not required when using port 80 or 443")
        ("http-tls-key",
            po::value<string>(&config.http_tls_key)->default_value(config.http_tls_key),
            "TLS key for the embedded HTTP server")
        ("http-tls-cert",
            po::value<string>(&config.http_tls_cert)->default_value(config.http_tls_cert),
            "TLS cert for the embedded HTTP server")
        ("http-num-threads",
            po::value<size_t>(&config.num_http_threads)->default_value(config.num_http_threads),
            "Threads for the embedded HTTP server")

        ;

    po::options_description cmdline_options;
    cmdline_options.add(general).add(http);
    po::variables_map vm;
    po::store(po::command_line_parser(argc, argv).options(cmdline_options).run(), vm);
    po::notify(vm);
    if (vm.count("help")) {
        std::cout << filesystem::path(argv[0]).stem().string() << " [options]";
        std::cout << cmdline_options << std::endl;
        return -1;
    }

    if (vm.count("version")) {
        std::cout << filesystem::path(argv[0]).stem().string() << ' '  << HttpServer::version() << endl;
        return -2;
    }

    auto llevel = logfault::LogLevel::INFO;
    if (log_level == "debug") {
        llevel = logfault::LogLevel::DEBUGGING;
    } else if (log_level == "trace") {
        llevel = logfault::LogLevel::TRACE;
    } else if (log_level == "info") {
        ;  // Do nothing
    } else {
        std::cerr << "Unknown log-level: " << log_level << endl;
        return -1;
    }

    logfault::LogManager::Instance().AddHandler(
                make_unique<logfault::StreamHandler>(clog, llevel));

    LOG_INFO << filesystem::path(argv[0]).stem().string() << ' ' << YAHAT_VERSION  " starting up. Log level: " << log_level;

    Logger::Instance().SetLogLevel(llevel);
    Logger::Instance().SetHandler([](LogLevel level,
                                  const std::string& msg) {
        static const std::array<std::string, 6> levels = {"NONE", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"};

        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

        std::clog << std::put_time(std::localtime(&now), "%c") << ' '
                  << levels.at(static_cast<size_t>(level))
                  << ' ' << std::this_thread::get_id() << ' '
                  << msg << std::endl;
    });

    try {
        HttpServer apiserver{config, [](const AuthReq& ar) {
            Auth auth;
            LOG_DEBUG << "Authenticating - auth header: " << ar.auth_header;
            auth.access = true;
            auth.account = "nobody";
            return auth;
        }};


        apiserver.addRoute("/api/v1", make_shared<MyRequestHandler>());
        apiserver.run();

    } catch (const exception& ex) {
        LOG_ERROR << "Caught exception from engine: " << ex.what();
        return 1;
    }

    return 0;
} // main
