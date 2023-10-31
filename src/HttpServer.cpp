
#include <fstream>

#include <boost/beast/http/string_body.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/beast/version.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/scope_exit.hpp>

#include "yahat/logging.h"
#include "yahat/HttpServer.h"

using namespace std;
using namespace std;
using boost::asio::ip::tcp;
namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace ssl = boost::asio::ssl;
namespace net = boost::asio;            // from <boost/asio.hpp>
using namespace std::placeholders;
using namespace std::string_literals;

ostream& operator << (ostream& o, const yahat::Request::Type& t) {
    static const array<string, 6> types = {"GET", "PUT", "PATCH", "POST", "DELETE", "OPTIONS"};

    return o << types.at(static_cast<size_t>(t));
}

namespace yahat {

namespace {

string generateUuid() {
    static boost::uuids::random_generator uuid_gen_;
    return boost::uuids::to_string(uuid_gen_());
}

struct LogRequest {
    LogRequest() = delete;
    LogRequest(const LogRequest& ) = delete;
    LogRequest(LogRequest&& ) = delete;
    LogRequest(const Request& r)
        : type{r.type}
        , uuid{r.uuid} {}

    boost::asio::ip::tcp::endpoint local, remote;
    Request::Type type;
    string location;
    string_view user;
    int replyValue = 0;
    string replyText;
    boost::uuids::uuid uuid;

private:
    std::once_flag done_;

public:

    template <typename T>
    void set(const T& res) {
        replyValue = res.result_int();
        replyText = res.reason();
        flush();
    }

    void flush() {
        call_once(done_, [&] {
            LOG_INFO << uuid << ' ' << remote << " --> " << local << " [" << user << "] " << type << ' ' << location.data() << ' ' << replyValue << " \"" << replyText << '"';
        });
    }

    void cancel() {
        call_once(done_, [&] {
            LOG_TRACE << "Log event cancelled";
        });
    }

    ~LogRequest() {
        flush();
    }
};

auto to_type(const http::verb& verb) {
    switch(verb) {
    case http::verb::get:
        return Request::Type::GET;
    case http::verb::post:
        return Request::Type::POST;
    case http::verb::patch:
        return Request::Type::PATCH;
    case http::verb::put:
        return Request::Type::PUT;
    case http::verb::delete_:
        return Request::Type::DELETE;
    case http::verb::options:
        return Request::Type::OPTIONS;
    default:
        throw runtime_error{"Unknown verb"};
    }
}

template <typename T>
auto makeReply(HttpServer& server, T&res, const Response& r, bool closeConnection, LogRequest& lr, Request::Type rt) {

    if (rt != Request::Type::OPTIONS) {
        if (r.body.empty()) {
            // Use the http code and reason to compose a json reply
            res.body() = r.responseStatusAsJson();
            auto mime = Response::getMimeType();
            res.base().set(http::field::content_type, mime);
        } else {
            res.body() = r.body;
            auto mime = r.mime_type;
            if (mime.empty()) {
                mime = r.mimeType(); // Try to get it from the context
            }
            if (mime.empty()) {
                // Use json as default
                // We are after all another REST API thing ;)
                mime = Response::getMimeType();
            }
            res.base().set(http::field::content_type, mime);
        }
    }
    res.result(r.code);
    res.reason(r.reason);
    res.base().set(http::field::server, server.serverId());
    res.base().set(http::field::connection, closeConnection ? "close" : "keep-alive");
    if (r.cors) {
        res.base().set(http::field::access_control_allow_origin, "*");
        res.base().set(http::field::access_control_allow_credentials, "true");
        res.base().set(http::field::access_control_allow_methods, "GET,OPTIONS,POST,PUT,PATCH,DELETE");
        res.base().set(http::field::access_control_allow_headers, "Authorization, Content-Encoding, Access-Control-Allow-Headers, Origin, Accept, X-Requested-With, Content-Type, Access-Control-Request-Method, Access-Control-Request-Headers");
    }

    if (auto mime = r.mimeType(); !mime.empty()) {
        res.base().set(http::field::content_type, {mime.data(), mime.size()});
    }
    res.prepare_payload();
    lr.set(res);
}

template <bool isTls, typename streamT>
void DoSession(streamT& streamPtr,
               HttpServer& instance,
               boost::asio::yield_context& yield)
{
    assert(streamPtr);
    auto& stream = *streamPtr;

    LOG_TRACE << "Processing session: " << beast::get_lowest_layer(stream).socket().remote_endpoint()
              << " --> " << beast::get_lowest_layer(stream).socket().local_endpoint();

    bool close = false;
    beast::error_code ec;
    beast::flat_buffer buffer{1024 * 64};

    if constexpr(isTls) {
        beast::get_lowest_layer(stream).expires_after(chrono::seconds(5));
        stream.async_handshake(ssl::stream_base::server, yield[ec]);
        if(ec) {
            LOG_ERROR << "TLS handshake failed: " << ec.message();
            return;
        }
    }

    while(!close) {
        LOG_TRACE << "Start of loop - close=" << close;

        beast::get_lowest_layer(stream).expires_after(chrono::seconds(instance.config().http_io_timeout));
        http::request<http::string_body> req;
        auto bytes = http::async_read(stream, buffer, req, yield[ec]);
        if(ec == http::error::end_of_stream) {
            LOG_TRACE << "Exiting loop end_of_stream";
            break;
        }
        if(ec) {
            LOG_ERROR << "read failed: " << ec.message();
            break;
        }

        if (!req.keep_alive()) {
            close = true;
        }

        // TODO: Check that the client accepts our json reply
        Request request{req.base().target(), req.body(), to_type(req.base().method()), &yield};

        LogRequest lr{request};
        lr.remote =  beast::get_lowest_layer(stream).socket().remote_endpoint();
        lr.local = beast::get_lowest_layer(stream).socket().local_endpoint();
        lr.location = req.base().target();

        if (const auto& ah = instance.authenticator()) {
            AuthReq ar{request, yield};
            if (auto it = req.base().find(http::field::authorization) ; it != req.base().end()) {
                auto [a, u] = instance.Authenticate({it->value().data(), it->value().size()});
                ar.auth_header = {it->value().data(), it->value().size()};
            }

            request.auth = ah(ar);
            lr.user = request.auth.account;
        }

        if (request.type == Request::Type::OPTIONS && instance.config() .auto_handle_cors) {
            LOG_TRACE << "This is an OPTIONS request. Just returning a dummy CORS reply";
            Response r{200, "OK"};
            r.cors = true;
            http::response<http::string_body> res;
            res.base().set(http::field::server, instance.serverId());
            makeReply(instance, res, r, close, lr, request.type);
            http::async_write(stream, res, yield[ec]);
            if(ec) {
                LOG_ERROR << "write failed: " << ec.message();
            }

            continue;
        }

        if (!request.auth.access) {
            LOG_TRACE << "Request was unauthorized!";

            Response r{401, "Access Denied!"};
            http::response<http::string_body> res;
            res.base().set(http::field::server, instance.serverId());
            if (instance.config().enable_http_basic_auth) {
                if (auto realm = instance.config().http_basic_auth_realm; !realm.empty()) {
                    res.base().set(http::field::www_authenticate, "Basic realm="s + realm);
                } else {
                    res.base().set(http::field::www_authenticate, "Basic");
                }
            }
            makeReply(instance, res, r, close, lr, request.type);
            http::async_write(stream, res, yield[ec]);
            if(ec) {
                LOG_ERROR << "write failed: " << ec.message();
            }

            continue;
        }

        if (!req.body().empty()) {
            if (auto it = req.base().find(http::field::content_type) ; it != req.base().end()) {
                // TODO: Check that the type is json
                LOG_TRACE << "Request has content type: " << it->value();
            }
        }

        bool sse_initialized = false;
        optional<http::response_serializer<http::empty_body>> sse_sr;
        struct EosData {
            array<char, 1> buffer;
            atomic_bool ok{true};
            boost::uuids::uuid uuid;
            std::function<void()> notify_connection_closed;
        };

        // Needed if the request is a SSE subscription
        auto eos_data = make_shared<EosData>();
        eos_data->uuid = request.uuid;

        // Setup support for SSE
        request.sse_send = [&](string_view sse) {
            beast::get_lowest_layer(stream).expires_after(std::chrono::seconds(instance.config().http_io_timeout));
            boost::system::error_code ec;

            if (!sse_initialized) {
               LOG_TRACE << "Initializing SSE for request " << request.uuid;
               http::response<http::empty_body>  res{http::status::ok, 11};
               res.set(http::field::server, "yahat "s + YAHAT_VERSION);
               res.set(http::field::content_type, "text/event-stream");
               res.set(http::field::keep_alive, "true");
               res.chunked(true);
               sse_sr.emplace(res);

               http::async_write_header(stream, *sse_sr, yield[ec]);
               if (ec) {
                   LOG_DEBUG << "Request " << request.uuid
                             << " - failed to send SSE header: " << ec;
                   return false;
               }

               sse_initialized = true;

               // Set up a callback for the read-direction to make sure that
               // we detect if the SSE connection is closed while it is idle.

               boost::asio::mutable_buffer rbb{eos_data->buffer.data(), eos_data->buffer.size()};
               auto uuid = request.uuid;

               if (request.notify_connection_closed) {
                   LOG_TRACE << "Added notify_connection_closed while setting up SSE";
                   eos_data->notify_connection_closed = std::move(request.notify_connection_closed);
               }

               boost::asio::async_read(stream, rbb, [eos_data](boost::system::error_code ec, size_t) {
                   LOG_DEBUG << "Request " << eos_data->uuid
                             << " - Read handler called: " << ec;
                   eos_data->ok = false;
                   if (eos_data->notify_connection_closed) {
                       eos_data->notify_connection_closed();
                   }
               });
            }

            if (!sse.empty()) {
                boost::asio::const_buffer b{sse.data(), sse.size()};
                boost::beast::net::async_write(stream, http::make_chunk(b), yield[ec]);

                if (ec) {
                    LOG_DEBUG << "Request " << request.uuid
                              << " - failed to send SSE payload: " << ec;
                    return false;
                }
            }

            return true;
        };

        request.probe_connection_ok = [eos_data]() {
            return eos_data && eos_data->ok;
        };

        const auto reply = instance.onRequest(request);
        if (reply.close) {
            close = true;
        }

        LOG_TRACE << "Preparing reply";
        http::response<http::string_body> res;
        makeReply(instance, res, reply, close, lr, request.type);
        http::async_write(stream, res, yield[ec]);
        if(ec) {
            LOG_WARN << "write failed: " << ec.message();
            return;
        }

        LOG_TRACE << "End of loop";
    }

    if constexpr(isTls) {
        beast::get_lowest_layer(stream).expires_after(chrono::seconds(instance.config().http_io_timeout));

        // Perform the SSL shutdown
        stream.async_shutdown(yield[ec]);
        if (ec) {
            LOG_TRACE << "TLS shutdown failed: " << ec.message();
        }
    } else {
        // Send a TCP shutdown
        stream.socket().shutdown(tcp::socket::shutdown_send, ec);
    }
}

} // ns

void Request::init(const string& undecodedTtarget)
{
#ifdef USING_BOOST_URL
    auto url = boost::urls::parse_origin_form(undecodedTtarget).value();
    target = url.path();
    all_arguments = url.query();
    string_view aa = all_arguments;
    for(auto pos = aa.find('&'); !aa.empty(); pos = aa.find('&')) {
        string_view current;
        if (pos == string_view::npos) {
            current = aa;
            aa = {};
        } else {
            current = aa.substr(0, pos);
            aa = aa.substr(pos + 1);
        }

        if (auto eq = current.find('='); eq != string_view::npos) {
            auto key = current.substr(0, eq);
            auto val = current.substr(eq +1);
            arguments[key] = val;
        } else {
            arguments[current] = {};
        }
    }
#else
    target = undecodedTtarget;
    if (auto pos = target.find('?'); pos != string::npos) {
        auto all_args = target.substr(pos + 1);
        target = target.substr(0, pos);
    }
#endif
}

HttpServer::HttpServer(const HttpConfig &config, authenticator_t authHandler, const std::string& branding)
    : config_{config}, authenticator_(std::move(authHandler))
    , server_{branding.empty() ? "yahat "s + YAHAT_VERSION : branding + "/yahat "s + YAHAT_VERSION}
{
}

std::future<void> HttpServer::start()
{

    // Start listening
    tcp::resolver resolver(ctx_);

    const bool is_tls = !config_.http_tls_key.empty();

    auto port = config_.http_port;
    if (port.empty()) {
        if (is_tls) {
            port = "https";
        } else {
            port = "http";
        }
    }

    LOG_DEBUG << "Preparing to listen to: "
              << config_.http_endpoint << " on "
              << (is_tls ? "HTTPS" : "HTTP")
              << " port " << port;

    auto endpoint = resolver.resolve({config_.http_endpoint, port});
    tcp::resolver::iterator end;
    for(; endpoint != end; ++endpoint) {
        tcp::endpoint ep = endpoint->endpoint();
        LOG_INFO << "Starting " << (is_tls ? "HTTPS" : "HTTP") << " endpoint: " << ep;

        boost::asio::spawn(ctx_, [this, ep, is_tls] (boost::asio::yield_context yield) {
            beast::error_code ec;

            tcp::acceptor acceptor{ctx_};
            acceptor.open(ep.protocol(), ec);
            if(ec) {
                LOG_ERROR << "Failed to open endpoint " << ep << ": " << ec;
                return;
            }

            acceptor.set_option(net::socket_base::reuse_address(true), ec);
            if(ec) {
                LOG_ERROR << "Failed to set option reuse_address on " << ep << ": " << ec;
                return;
            }

            auto sslCtx = make_shared<ssl::context>(ssl::context::tls_server);
            if (is_tls) {
                try {
                    sslCtx->use_certificate_chain_file(config_.http_tls_cert);
                    sslCtx->use_private_key_file(config_.http_tls_key, ssl::context::pem);
                } catch(const exception& ex) {
                    LOG_ERROR << "Failed to initialize tls context: " << ex.what();
                    return;
                }
            }

            // Bind to the server address
            acceptor.bind(ep, ec);
            if(ec) {
                LOG_ERROR << "Failed to bind to " << ep << ": " << ec;
                return;
            }

            // Start listening for connections
            acceptor.listen(net::socket_base::max_listen_connections, ec);
            if(ec) {
                LOG_ERROR << "Failed to listen to on " << ep << ": " << ec;
                return;
            }

            size_t errorCnt = 0;
            const size_t maxErrors = 64;
            for(;!ctx_.stopped() && errorCnt < maxErrors;) {
                tcp::socket socket{ctx_};
                acceptor.async_accept(socket, yield[ec]);
                if(ec) {
                    // I'm unsure about how to deal with errors here.
                    // For now, allow `maxErrors` to occur before giving up
                    LOG_WARN << "Failed to accept on " << ep << ": " << ec;
                    ++errorCnt;
                    continue;
                }

                errorCnt = 0;

                if (is_tls) {
                    boost::asio::spawn(acceptor.get_executor(), [this, sslCtx, socket=std::move(socket)](boost::asio::yield_context yield) mutable {
                        auto stream = make_shared<beast::ssl_stream<beast::tcp_stream>>(std::move(socket), *sslCtx);
                        try {
                            DoSession<true>(stream, *this, yield);
                        } catch(const exception& ex) {
                            LOG_ERROR << "Caught exception from DoSession [HTTPS]: " << ex.what();
                        }
                    });

                } else {
                    boost::asio::spawn(acceptor.get_executor(), [this, socket=std::move(socket)](boost::asio::yield_context yield) mutable {
                        auto stream = make_shared<beast::tcp_stream>(std::move(socket));
                        try {
                            DoSession<false>(stream, *this, yield);
                        } catch(const exception& ex) {
                            LOG_ERROR << "Caught exception from DoSession [HTTP]: " << ex.what();
                        }
                    });
                }
            }

        });
    }; // for resolver endpoint


    startWorkers();
    return promise_.get_future();
}

void HttpServer::run()
{
    LOG_DEBUG << "Starting the HTTP server...";

    BOOST_SCOPE_EXIT(void) {
        LOG_DEBUG << "The HTTP server is done.";
    } BOOST_SCOPE_EXIT_END

        auto future = start();
    future.get();
}

string_view HttpServer::version() noexcept
{
    return YAHAT_VERSION;
}

void HttpServer::stop()
{
    ctx_.stop();
    for(auto& worker : workers_) {
        worker.join();
    }
    promise_.set_value();
}

void HttpServer::addRoute(std::string_view target, handler_t handler)
{
    if (target.size() == 0) {
        throw runtime_error{"A target's route cannot be empty"};
    }
    string key{target};
    routes_[std::move(key)] = handler;
}

std::pair<bool, string_view> HttpServer::Authenticate(const std::string_view &/*authHeader*/)
{
    // TODO: Implement
    static const string_view teste{"teste"};
    return {true, teste};
}

Response HttpServer::onRequest(Request &req) noexcept
{
    // Find the route!
    string_view tw{req.target.data(), req.target.size()};

    RequestHandler *best_handler = {};
    string_view best_route;

    for(const auto& [route, handler] : routes_) {
        const auto len = route.size();

        // Target must be at least the lenght of the route
        if (tw.size() < len) {
            continue;
        }

        // Target is only relevant if it's the same size as the route
        // or if it has a slash at the location where target ends
        if (tw.size() == len || tw.at(len) == '/') {
            auto relevant = tw.substr(0, len);
            if (relevant == route) {
                // We need the longest possible match
                if (!best_handler || (best_route.size() < route.size())) {
                    best_handler = handler.get();
                    best_route = route;
                }
            }
        }
    }

    if (best_handler) {
        try {
            LOG_TRACE << "Found route '" << best_route << "' for target '" << tw << "'";
            req.route = best_route;
            return best_handler->onReqest(req);
        } catch(const Response& resp) {
            return resp;
        } catch (const exception& ex) {
            LOG_ERROR << "Caught unexpected exception "
                      << typeid(ex).name() << " from request: " << ex.what();
            return {500, "Internal server error"};
        }
    }

    return {404, "Document not found"};
}

void HttpServer::startWorkers()
{
    for(size_t i = 0; i < config_.num_http_threads; ++i) {
        workers_.emplace_back([this, i] {
                LOG_DEBUG << "HTTP worker thread #" << i << " starting up.";
                try {
                    ctx_.run();
                } catch(const exception& ex) {
                    LOG_ERROR << "HTTP worker #" << i
                              << " caught exception: "
                              << ex.what();
                }
                LOG_DEBUG << "HTTP worker thread #" << i << " done.";
        });
    }
}

HttpServer::FileHandler::FileHandler(std::filesystem::path root)
    : root_{root}
{
    LOG_DEBUG << "Ready to serve path: " << root;
}

Response HttpServer::FileHandler::onReqest(const Request &req)
{
    static const Response not_found{404, "Document not found"};
    auto path = resolve(req.target);

    std::error_code ec;
    auto what = filesystem::status(path, ec);
    if (ec) {
        LOG_DEBUG << "Path " << path << ": " << ec.message();
        return not_found;
    }

    if (what.type() == filesystem::file_type::regular) {
        return readFile(path);
    }

    if (what.type() == filesystem::file_type::directory) {
        return handleDir(path);
    }


    return not_found;
}

std::filesystem::path HttpServer::FileHandler::resolve(std::string_view target)
{

    while(!target.empty() && target[0] == '/') {
        target = target.substr(1);
    }

    auto t = filesystem::path{target}.lexically_normal();
    if (!t.empty() &&  t.native().front() == '/') {
        throw runtime_error{"Invalid target. Normalized target cannot start with slash!"};
    }

    auto raw = root_;

    if (!target.empty()) {
        raw /= t;
    }

    auto r = raw.lexically_normal();

    // Remove trailing slash. It sometimes occur...
    if (auto& n = r.native() ; !n.empty()) {
        if (n.back() == '/') {
            r = {r.native().substr(0, r.native().size() -1)};
        }
    }

    // validate
    auto a = root_.begin();
    auto b = r.begin();
    for(; a != root_.end(); ++a, ++b) {
        if (b == r.end()) {
            throw runtime_error{"Invalid target. Tries to access filesystem above root level"};
        }
        if (*b != *a) {
            throw runtime_error{"Invalid target. Tries to access filesystem outside root path"};
        }
    }

    return r;
}

Response HttpServer::FileHandler::readFile(const std::filesystem::path &path)
{
    ifstream file{path};
    if (file.is_open()) {
        Response r;
        const auto len = filesystem::file_size(path);
        r.body.resize(len);
        file.read(r.body.data(), len);
        r.target = path.string();
        return r;
    }

    return {500, "Failed to open file for read"};
}

Response HttpServer::FileHandler::handleDir(const std::filesystem::path &path)
{
    auto index = path;
    index /= "index.html";
    if (filesystem::is_regular_file(index)) {
        return readFile(index);
    }

    return listDir(path);
}

Response HttpServer::FileHandler::listDir(const std::filesystem::path &path)
{
    return {404, "Directoty listings are not supported"};
}

string_view Response::getMimeType(string_view type)
{
    static const std::unordered_map<string_view, string_view> mime_types = {
        {"json", "application/json; charset=utf-8"},
        {"bin", "application/octet-stream"},
        {"bz", "application/x-bzip"},
        {"bz2", "application/x-bzip2"},
        {"css", "text/css"},
        {"csv", "text/csv"},
        {"gz", "application/gzip"},
        {"gif", "image/gif"},
        {"htm", "text/html"},
        {"html", "text/html"},
        {"ico", "image/vnd.microsoft.icon"},
        {"jar", "application/java-archive"},
        {"jpeg", "image/jpeg"},
        {"jpg", "image/jpeg"},
        {"js", "text/javascript"},
        {"mjs", "text/javascript"},
        {"otf", "font/otf"},
        {"png", "image/png"},
        {"svg", "image/svg+xml"},
        {"tar", "application/x-tar"},
        {"tiff", "image/tiff"},
        {"ttf", "font/ttf"},
        {"txt", "text/plain; charset=utf-8"},
        {"xhtml", "application/xhtml+xml"},
        {"xml", "application/xml"},
        {"zip", "application/zip"},
        {"7z", "application/x-7z-compressed"},
        {"jsonld", "application/ld+json"}
    };

    if (auto mt = mime_types.find(type); mt != mime_types.end()) {
        return mt->second;
    }

    return {};
}


string_view Response::mimeType() const
{
    if (!mime_type.empty()) {
        return mime_type;
    }

    if (!target.empty()) {
        if (const auto pos = target.find_last_of('.'); pos != string::npos) {
            if (target.size() > (pos + 1)) {
                const auto type = target.substr(pos + 1);
                return getMimeType(type);
            }
        }
    }

    return {};
}

boost::uuids::uuid generateUuid()
{
    static boost::uuids::random_generator uuid_gen;
    return uuid_gen();
}



} // ns
