
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>
#include <algorithm>

#define ZLIB_CONST
#include <zlib.h>

#include <boost/beast/http/string_body.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/beast/version.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/random_generator.hpp>

#include "yahat/logging.h"
#include "yahat/HttpServer.h"
#include "yahat/YahatInstanceMetrics.h"

using namespace std;
using namespace std;
using boost::asio::ip::tcp;
namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace ssl = boost::asio::ssl;
namespace net = boost::asio;            // from <boost/asio.hpp>
using namespace std::placeholders;
using namespace std::string_literals;


namespace   {

// Helper function to trim whitespace from a std::string_view
constexpr std::string_view trim(std::string_view str) {
    while (!str.empty() && std::isspace(str.front())) {
        str.remove_prefix(1);
    }
    while (!str.empty() && std::isspace(str.back())) {
        str.remove_suffix(1);
    }
    return str;
}

// Function to create a lazy view over the cookies
constexpr auto parse_cookies(std::string_view cookie_header) {
    return cookie_header
           // Split the header into individual cookie key-value pairs by ';'
           | std::views::split(';')
           // Transform each key-value pair into a std::pair<std::string_view, std::string_view>
           | std::views::transform([](auto &&cookie) -> std::pair<std::string_view, std::string_view> {
                 // Convert the range to a std::string_view
                 std::string_view cookie_str = std::string_view(std::ranges::begin(cookie), std::ranges::end(cookie));
                 // Find the '=' separator
                 auto pos = cookie_str.find('=');
                 if (pos == std::string_view::npos) {
                     // If there's no '=', treat it as a key with an empty value
                     return {trim(cookie_str), std::string_view{}};
                 }
                 // Split into key and value, trimming both
                 std::string_view key = trim(cookie_str.substr(0, pos));
                 std::string_view value = trim(cookie_str.substr(pos + 1));
                 return {key, value};
             });
}

string_view toString(yahat::Request::Type type) {
    static constexpr auto types = to_array({"GET", "PUT", "PATCH", "POST", "DELETE", "OPTIONS"});
    return types.at(static_cast<size_t>(type));
}

string decompressGzip(string_view compressedData, size_t maxDecompressedBytes) {
    z_stream zs{};
    string decompressed_data;
    decompressed_data.reserve(compressedData.size() * 2);

    // Initialize zlib for decompression (inflate)
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) {
        throw std::runtime_error("inflateInit2 failed");
    }

    zs.next_in = reinterpret_cast<const Bytef*>(compressedData.data());
    zs.avail_in = compressedData.size();

    int ret{};
    array<char, 4096> buffer{};
    decompressed_data.clear();

    do {
        zs.next_out = reinterpret_cast<unsigned char*>(buffer.data());
        zs.avail_out = buffer.size();

        ret = inflate(&zs, 0);

        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            inflateEnd(&zs);
            throw std::runtime_error("Decompression error");
        }

        decompressed_data.append(buffer.data(), buffer.size() - zs.avail_out);

        // Check the size limit to prevent decompression bombs
        if (decompressed_data.size() > maxDecompressedBytes) {
            inflateEnd(&zs);
            throw std::runtime_error("Decompressed data exceeds maximum allowed size");
        }

    } while (ret != Z_STREAM_END);

    inflateEnd(&zs);
    return decompressed_data;
}

string compressGzip(string_view input) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    string compressed_output;
    compressed_output.reserve(input.size());

    // Initialize zlib for compression (deflate)
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("deflateInit2 failed");
    }

    zs.next_in = reinterpret_cast<const unsigned char*>(input.data());
    zs.avail_in = input.size();
    array<char, 4096> buffer{};
    int ret{};

    do {
        zs.next_out = reinterpret_cast<unsigned char*>(buffer.data());
        zs.avail_out = buffer.size();

        ret = deflate(&zs, Z_FINISH);

        if (ret == Z_STREAM_ERROR) {
            deflateEnd(&zs);
            throw std::runtime_error("Compression error");
        }

        compressed_output.append(buffer.data(), buffer.size() - zs.avail_out);

    } while (ret != Z_STREAM_END);

    deflateEnd(&zs);
    return compressed_output;
}


} // anon ns

ostream& operator << (ostream& o, const yahat::Request::Type& t) {
    return o << toString(t);
}

namespace yahat {

#ifdef YAHAT_ENABLE_METRICS
void RequestHandler::enableMetrics(HttpServer& server, std::string_view target)
{
    Metrics::labels_t labels;
    labels.emplace_back("target", target);
    std::vector<double> bb = {0.00001, 0.00005, 0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 1.0, 3.0};

    metrics_ = server.internalMetrics()->metrics().AddHistogram(
        "yahat_http_request_duration",
        "The duration of HTTP requests",
        "sec",
        labels,
        bb);
}
#endif


void SseHandler::prepareResponse(const Response& response) {
    if (response.cors) {
        headers.set(http::field::access_control_allow_origin, "*");
        headers.set(http::field::access_control_allow_credentials, "true");
        headers.set(http::field::access_control_allow_methods, "GET,OPTIONS,POST,PUT,PATCH,DELETE");
        headers.set(http::field::access_control_allow_headers,
                    "Authorization, Content-Encoding, Access-Control-Allow-Headers, Origin, Accept, X-Requested-With, Content-Type, Access-Control-Request-Method, Access-Control-Request-Headers");
    }
    response.applyHeaders(headers);
    for (const auto& cookie : response.cookies) {
        Response value;
        value.headers.set(http::field::set_cookie, format("{}={}", cookie.first, cookie.second));
        http::fields validated;
        value.applyHeaders(validated);
        headers.insert(http::field::set_cookie, validated[http::field::set_cookie]);
    }
}

bool SseHandler::send(std::string_view message)
{
    if (eos_data_->closed) {
        return false;
    }
    stream_->disable_timeout();
    boost::system::error_code ec;

    if (!sse_initialized_) {
        LOG_TRACE << "Initializing SSE...";
        http::response<http::empty_body>  res{http::status::ok, 11};
        res.set(http::field::server, "yahat "s + YAHAT_VERSION);
        res.set(http::field::content_type, "text/event-stream");
        // Continuations close the connection after their final chunk. Tell
        // clients before they try reusing it for another HTTP request.
        res.keep_alive(false);
        res.chunked(true);
        Response extra_headers;
        extra_headers.headers = headers;
        extra_headers.applyHeaders(res.base());
        http::response_serializer<http::empty_body> serializer{res};

        if (!stream_->async_write_header(serializer)) {
            return false;
        }

        sse_initialized_ = true;

        // Set up a callback for the read-direction to make sure that
        // we detect if the SSE connection is closed while it is idle.

        auto state = eos_data_;
        boost::asio::mutable_buffer rbb{state->buffer.data(), state->buffer.size()};
        stream_->async_read_stream(rbb, [state](boost::system::error_code ec, size_t) {
            if (ec == net::error::operation_aborted) return;
            // Incoming bytes are unexpected once this response becomes SSE.
            LOG_DEBUG << "SSE connection closed or received unexpected input: " << ec.message();
            if (!state->closed.exchange(true) && state->notify_connection_closed) {
                state->notify_connection_closed();
            }
        });
    }

    if (!message.empty()) {
        LOG_TRACE << "Sending SSE message (" << message.size() << " bytes)";
        boost::asio::const_buffer b{message.data(), message.size()};
        auto c = http::make_chunk(b);
        if (!stream_->async_write_stream(http::make_chunk(b))) {
            return false;
        }
    }

    return true;
};

struct SseQueueHandler::QueueState {
    explicit QueueState(boost::asio::io_context& ctx) : timer{ctx} {}
    boost::asio::steady_timer timer;
    std::atomic_bool active{false};
    bool closed = false;
    bool finishing = false;
    bool wake_pending = false;
    size_t queued_bytes = 0;
    size_t max_bytes = 0;
    std::chrono::seconds heartbeat{0};
    std::queue<std::string> queue;
    std::mutex mutex;
    std::optional<boost::asio::any_io_executor> executor;

    // Called with mutex held. All timer operations run on the session strand.
    static void wake(const std::shared_ptr<QueueState>& state) {
        if (state->executor && !state->wake_pending) {
            state->wake_pending = true;
            boost::asio::post(*state->executor, [state] {
                std::lock_guard lock{state->mutex};
                state->wake_pending = false;
                state->timer.cancel();
            });
        }
    }
};

SseQueueHandler::SseQueueHandler(HttpServer &server)
    : SseHandler{server}, state_{std::make_shared<QueueState>(server.getCtx())} {
    std::weak_ptr<QueueState> weak = state_;
    setOnConnectionClosed([weak] {
        if (auto state = weak.lock()) {
            std::lock_guard lock{state->mutex};
            state->closed = true;
            state->active = false;
            QueueState::wake(state);
        }
    });
}

SseQueueHandler::~SseQueueHandler() {
    closeSse();
}

bool SseQueueHandler::active() const noexcept {
    return state_->active;
}

void SseQueueHandler::setQueueLimit(size_t max_bytes) {
    std::lock_guard lock{state_->mutex};
    if (max_bytes && state_->queued_bytes > max_bytes) {
        throw std::length_error{"SSE queue already exceeds requested limit"};
    }
    state_->max_bytes = max_bytes;
}

void SseQueueHandler::setHeartbeatInterval(std::chrono::seconds interval) {
    if (interval.count() < 0) throw std::invalid_argument{"Negative SSE heartbeat interval"};
    std::lock_guard lock{state_->mutex};
    state_->heartbeat = interval;
    QueueState::wake(state_);
}

void SseQueueHandler::sendSse(std::string message) {
    std::lock_guard lock{state_->mutex};
    if (state_->closed || state_->finishing) {
        return;
    }
    if (state_->max_bytes && message.size() > state_->max_bytes - state_->queued_bytes) {
        LOG_WARN << "SSE queue capacity exceeded";
        throw std::length_error{"SSE queue capacity exceeded"};
    }
    state_->queue.push(std::move(message));
    state_->queued_bytes += state_->queue.back().size();
    QueueState::wake(state_);
}

void SseQueueHandler::sendSse(std::string_view eventName, std::string_view data)
{
    if (eventName.empty() || eventName.find_first_of("\r\n") != std::string_view::npos
        || data.find_first_of("\r\n") != std::string_view::npos) {
        throw std::invalid_argument{"SSE event name and data must be single lines"};
    }
    sendSse(format("event: {}\ndata: {}\n\n", eventName, data));
}

void SseQueueHandler::closeSse() {
    std::lock_guard lock{state_->mutex};
    state_->closed = true;
    state_->active = false;
    QueueState::wake(state_);
}

void SseQueueHandler::finishSse() {
    std::lock_guard lock{state_->mutex};
    state_->finishing = true;
    QueueState::wake(state_);
}

Response SseQueueHandler::proceeding() {
    auto state = state_;
    {
        std::lock_guard lock{state->mutex};
        state->executor = getYield().get_executor();
        state->active = !state->closed;
    }
    LOG_DEBUG << "Starting queued SSE response";
    if (state->active && !send("")) closeSse();
    while (state->active) {
        if (auto message = next()) {
            if (!send(*message)) {
                closeSse();
            }
            continue;
        }
        std::chrono::seconds heartbeat;
        {
            std::lock_guard lock{state->mutex};
            if (state->closed || (state->finishing && state->queue.empty())) {
                break;
            }
            heartbeat = state->heartbeat;
        }
        boost::system::error_code ec;
        state->timer.expires_after(heartbeat.count() ? heartbeat : std::chrono::seconds{30});
        state->timer.async_wait(getYield()[ec]);
        if (ec && ec != boost::asio::error::operation_aborted) {
            break;
        }
        if (!ec && heartbeat.count() && !send(":\n\n")) closeSse();
    }
    closeSse();
    LOG_DEBUG << "Queued SSE response finished";
    return {};
}

std::optional<string> SseQueueHandler::next() {
    std::lock_guard lock{state_->mutex};
    if (state_->closed || state_->queue.empty()) {
        return {};
    }
    auto message = std::move(state_->queue.front());
    state_->queue.pop();
    state_->queued_bytes -= message.size();
    return message;
}

namespace {

template <typename T>
struct ScopedExit {
    explicit ScopedExit(T&& fn)
        : fn_{std::move(fn)} {}

    ScopedExit(const ScopedExit&) = delete;
    ScopedExit(ScopedExit&&) = delete;

    ~ScopedExit() {
        fn_();
    }

    ScopedExit& operator =(const ScopedExit&) = delete;
    ScopedExit& operator =(ScopedExit&&) = delete;
private:
    T fn_;
};

template <typename T>
class StreamImpl : public yahat::Continuation::Stream {
public:
    explicit StreamImpl(std::shared_ptr<T> stream, boost::asio::yield_context& yield,
                        std::function<void()> disconnected)
        : owner_{std::move(stream)}, stream_{*owner_}, yield_{yield}
        , disconnected_{std::move(disconnected)}
        , read_state_{std::make_shared<ReadState>(yield.get_executor())} {}

    size_t async_read_stream(boost::asio::mutable_buffer buffer) override {
        boost::system::error_code ec;
        auto len = boost::asio::async_read(stream_, buffer, yield_[ec]);
        notifyDisconnected(ec);
        if (ec) {
            return 0;
        }
        return len;
    }

    void async_read_stream(boost::asio::mutable_buffer buffer,
                           std::function<void (const boost::system::error_code &, std::size_t)> handler) override {
        if (read_state_->pending) throw std::logic_error{"A stream read is already pending"};
        read_state_->pending = true;
        auto owner = owner_;
        auto disconnected = disconnected_;
        auto state = read_state_;
        boost::asio::async_read(stream_, buffer, boost::asio::bind_executor(yield_.get_executor(),
            [owner, disconnected, state, handler = std::move(handler)](const boost::system::error_code& ec, size_t bytes) {
                state->pending = false;
                state->done.cancel();
                try {
                    handler(ec, bytes);
                } catch (...) {
                    LOG_ERROR << "Exception in stream read callback";
                }
                if (ec && ec != net::error::operation_aborted && disconnected) {
                    try { disconnected(); }
                    catch (...) { LOG_ERROR << "Exception in disconnect callback"; }
                }
            }));
    }
    bool async_write_stream(boost::asio::const_buffer buffer) override {
        boost::system::error_code ec;
        boost::beast::net::async_write(stream_, buffer, yield_[ec]);
        notifyDisconnected(ec);
        return !ec;
    }
    bool async_write_stream(const chunk_t &chunk) override {
        boost::system::error_code ec;
        boost::beast::net::async_write(stream_, chunk, yield_[ec]);
        notifyDisconnected(ec);
        return !ec;
    }
    bool async_write_header(boost::beast::http::response_serializer<http::empty_body> &ser) override {
        boost::system::error_code ec;
        http::async_write_header(stream_, ser, yield_[ec]);
        notifyDisconnected(ec);
        return !ec;
    }
    bool finish_stream() override {
        boost::system::error_code ec;
        net::async_write(stream_, http::make_chunk_last(), yield_[ec]);
        notifyDisconnected(ec);
        return !ec;
    }
    // Await cancellation of the read monitor before starting TLS shutdown.
    void finishReads() {
        if (read_state_->pending) {
            boost::system::error_code ec;
            beast::get_lowest_layer(stream_).socket().cancel(ec);
            while (read_state_->pending) read_state_->done.async_wait(yield_[ec]);
        }
    }
    void set_timeout(chrono::seconds timeout) override {
        beast::get_lowest_layer(stream_).expires_after(timeout);
    }
    void disable_timeout() override {
        beast::get_lowest_layer(stream_).expires_never();
    }

private:
    struct ReadState {
        explicit ReadState(boost::asio::any_io_executor executor) : done{executor} {
            done.expires_at(std::chrono::steady_clock::time_point::max());
        }
        boost::asio::steady_timer done;
        bool pending = false; // Accessed only on the session strand.
    };
    void notifyDisconnected(const boost::system::error_code& ec) {
        if (ec && ec != net::error::operation_aborted && disconnected_) {
            try { disconnected_(); }
            catch (...) { LOG_ERROR << "Exception in disconnect callback"; }
        }
    }
    std::shared_ptr<T> owner_;
    T& stream_;
    boost::asio::yield_context& yield_;
    std::function<void()> disconnected_;
    std::shared_ptr<ReadState> read_state_;
};

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

    string_view body = r.body;
    string body_buffer;
    if (rt != Request::Type::OPTIONS) {
        if (r.body.empty() && r.generate_status_body) {
            // Use the http code and reason to compose a json reply
            body_buffer = r.responseStatusAsJson();
            body = body_buffer;
            auto mime = Response::getMimeType();
            res.base().set(http::field::content_type, mime);
        } else {
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

    if (!body.empty() && r.compression == Response::Compression::GZIP) {
        body_buffer = compressGzip(body);
        body = body_buffer;
        res.base().set(http::field::content_encoding, "gzip");
    }

    res.body() = body;
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
    // copy cookies from r.cookies to res
    for(const auto& c : r.cookies) {
        res.base().insert(http::field::set_cookie, format("{}={}", c.first, c.second));
    }

    if (auto mime = r.mimeType(); !mime.empty()) {
        res.base().set(http::field::content_type, {mime.data(), mime.size()});
    }
    r.applyHeaders(res.base());
    res.prepare_payload();
    lr.set(res);
}

template <bool isTls, typename streamT>
void DoSession(streamT& streamPtr,
               HttpServer& instance,
               boost::asio::yield_context& yield)
{
#ifdef YAHAT_ENABLE_METRICS
    YahatInstanceMetrics::gauge_scoped_t count_session;
    auto * metrics = instance.internalMetrics();
    if (metrics) {
        count_session = metrics->currentSessions()->scoped();
    }
#endif

    assert(streamPtr);
    auto& stream = *streamPtr;
    ScopedExit cancel_pending{[&stream] {
        boost::system::error_code ignored;
        beast::get_lowest_layer(stream).socket().cancel(ignored);
    }};

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
        http::request_parser<http::string_body> parser;
        if (const auto limit = instance.config().max_request_body_size) parser.body_limit(*limit);
        if (const auto limit = instance.config().max_request_header_size) parser.header_limit(*limit);
        auto bytes = http::async_read(stream, buffer, parser, yield[ec]);
        if(ec == http::error::end_of_stream) {
            LOG_TRACE << "Exiting loop end_of_stream";
            break;
        }
        if(ec) {
            LOG_ERROR << "read failed: " << ec.message();
            if (ec == http::error::body_limit || ec == http::error::header_limit) {
                // Reject before dispatch without retaining an oversized request.
                const auto status = ec == http::error::body_limit
                    ? http::status::payload_too_large : http::status::request_header_fields_too_large;
                http::response<http::empty_body> denied{status, 11};
                denied.keep_alive(false);
                denied.content_length(0);
                http::async_write(stream, denied, yield[ec]);
            }
            break;
        }

        auto req = parser.release();
        if (!req.keep_alive()) {
            close = true;
        }

#ifdef YAHAT_ENABLE_METRICS
        if (metrics) {
            metrics->incomingRequests()->inc();
        }
#endif

        // TODO: Check that the client accepts our json reply

        string req_body;
        if (req[http::field::content_encoding] == "gzip") {
            req_body = decompressGzip(req.body(), instance.config().max_decompressed_size);
        } else {
            req_body = req.body();
        }

        auto curr_req = make_shared<Request>(req.base().target(), std::move(req_body), to_type(req.base().method()), &yield, isTls);
        curr_req->headers = req.base();
        assert(curr_req);
        auto& request = *curr_req;

        // copy cookies
        const auto cookies = request.getHeader("Cookie");
        for(auto c : parse_cookies(cookies)) {
            request.cookies.emplace_back(c);
        }

        Response::Compression compression = Response::Compression::NONE;
        {
            string ae = req[http::field::accept_encoding];
            transform(ae.begin(), ae.end(), ae.begin(), ::tolower);
            if (ae.find("gzip") != std::string::npos) {
                compression = Response::Compression::GZIP;
            }
        }

        LogRequest lr{request};
        lr.remote =  beast::get_lowest_layer(stream).socket().remote_endpoint();
        lr.local = beast::get_lowest_layer(stream).socket().local_endpoint();
        lr.location = req.base().target();

        const auto& options = instance.routeOptions(request.target);
        const bool auto_cors = options.auto_handle_cors.value_or(instance.config().auto_handle_cors);
        auto configure_cors = [&](Response& reply) {
            reply.cors = reply.auto_handle_cors.value_or(auto_cors);
            if (reply.cors && options.allowed_origins && !request.getHeader("Origin").empty()) {
                // The policy has already validated this origin; errors for invalid
                // origins explicitly disable CORS below.
                reply.headers.set(http::field::access_control_allow_origin, request.getHeader("Origin"));
                reply.headers.insert(http::field::vary, "Origin");
            }
        };
        auto send_reply = [&](Response reply) {
            configure_cors(reply);
            reply.compression = compression;
            close = close || reply.close;
            http::response<http::string_body> res;
            makeReply(instance, res, reply, close, lr, request.type);
            http::async_write(stream, res, yield[ec]);
            if (ec) {
                close = true;
                LOG_DEBUG << "HTTP response write failed: " << ec.message();
            }
        };

        // Origin validation precedes authentication and automatic preflight handling.
        const auto origins = request.headers.equal_range(http::field::origin);
        const bool invalid_origin = options.allowed_origins && origins.first != origins.second
            && (std::next(origins.first) != origins.second
                || std::find(options.allowed_origins->begin(), options.allowed_origins->end(),
                             request.getHeader("Origin")) == options.allowed_origins->end());
        if (invalid_origin) {
            LOG_DEBUG << "Rejecting HTTP request with disallowed Origin";
            Response denied{403, "Forbidden"};
            denied.auto_handle_cors = false;
            send_reply(std::move(denied));
            continue;
        }
        if (options.request_filter) {
            if (auto rejected = options.request_filter(request)) {
                send_reply(std::move(*rejected));
                continue;
            }
        }
        if (options.public_access) {
            request.auth.access = true;
        } else if (const auto& ah = instance.authenticator()) {
            AuthReq ar{request, yield};
            if (auto it = req.base().find(http::field::authorization) ; it != req.base().end()) {
                auto [a, u] = instance.Authenticate({it->value().data(), it->value().size()});
                ar.auth_header = {it->value().data(), it->value().size()};
            }

            request.auth = ah(ar);
            lr.user = request.auth.account;
        }

        if (request.type == Request::Type::OPTIONS && auto_cors) {
            LOG_TRACE << "This is an OPTIONS request. Just returning a dummy CORS reply";
            send_reply(Response{200, "OK"});

            continue;
        }

        if (!request.auth.access) {
            LOG_TRACE << "Request was unauthorized!";

            Response r = options.unauthorized_response
                ? options.unauthorized_response(request) : Response{401, "Unauthorized"};
            if (!options.unauthorized_response && instance.config().enable_http_basic_auth) {
                const auto& realm = instance.config().http_basic_auth_realm;
                r.headers.set(http::field::www_authenticate,
                              realm.empty() ? "Basic" : "Basic realm="s + realm);
            }
            send_reply(std::move(r));
            continue;
        }

        if (!req.body().empty()) {
            if (auto it = req.base().find(http::field::content_type) ; it != req.base().end()) {
                // TODO: Check that the type is json
                LOG_TRACE << "Request has content type: " << it->value();
            }
        }

        if (!options.allowed_methods.empty()
            && std::find(options.allowed_methods.begin(), options.allowed_methods.end(), request.type)
                == options.allowed_methods.end()) {
            Response denied{405, "Method Not Allowed"};
            std::string allow;
            for (const auto method : options.allowed_methods) {
                if (!allow.empty()) allow += ", ";
                allow += toString(method);
            }
            denied.headers.set(http::field::allow, allow);
            send_reply(std::move(denied));
            continue;
        }

        auto reply = instance.onRequest(request);
        if (reply.close) {
            close = true;
        }

        if (auto cont = reply.continuation()) {
            configure_cors(reply);
            cont->prepareResponse(reply);
            StreamImpl stream_wrapper(streamPtr, yield, [curr_req] {
                if (!curr_req->cancelled->exchange(true) && curr_req->notify_connection_closed) {
                    curr_req->notify_connection_closed();
                }
            });
            cont->proceed(stream_wrapper, yield);
            stream_wrapper.finishReads();
            close = true;
        } else {
            configure_cors(reply);
            reply.compression = compression;

            LOG_TRACE << "Preparing reply";
            http::response<http::string_body> res;
            makeReply(instance, res, reply, close, lr, request.type);
            http::async_write(stream, res, yield[ec]);
            if(ec) {
                LOG_WARN << "write failed: " << ec.message();
                return;
            }
        }

        LOG_TRACE << "End of loop";
    }

    // Cancel an SSE disconnect monitor even after normal stream completion.
    beast::get_lowest_layer(stream).socket().cancel(ec);
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
#ifdef YAHAT_ENABLE_METRICS
    if (config.enable_metrics) {
        metrics_ = make_shared<YahatInstanceMetrics>();

        LOG_INFO << "Metrics enabled at '" << config.metrics_target <<'\'';
        addRoute(config_.metrics_target, metrics_->metricsHandler(), "GET");
    }
#endif
}

#ifdef YAHAT_ENABLE_METRICS
HttpServer::HttpServer(const HttpConfig &config, authenticator_t authHandler, Metrics &metricsInstance, const std::string &branding)
: config_{config}, authenticator_(std::move(authHandler)), server_{branding.empty() ? "yahat "s + YAHAT_VERSION : branding + "/yahat "s + YAHAT_VERSION}
{
    metrics_ = make_shared<YahatInstanceMetrics>(&metricsInstance);

    addRoute(config_.metrics_target, metrics_->metricsHandler(), "GET");
    LOG_INFO << "Metrics enabled at '" << config.metrics_target <<'\'';
}
#endif

std::future<void> HttpServer::start()
{

    // Start listening
    tcp::resolver resolver(ctx_);

    const bool is_tls = !config_.http_tls_key.empty();

    string port = config_.http_port;
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

    auto results = resolver.resolve(config_.http_endpoint, port);
#if BOOST_VERSION >= 107000
    // Newer Boost (1.70 and later) using results_type
    for (const auto& result : results) {
        tcp::endpoint ep = result.endpoint();
#else
    // Older Boost (pre-1.70) using iterators
    tcp::resolver::iterator end;
    for (auto it = results.begin(); it != end; ++it) {
        tcp::endpoint ep = it->endpoint();

#endif
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
                tcp::socket socket{net::make_strand(ctx_)};
                acceptor.async_accept(socket, yield[ec]);
                if(ec) {
                    // I'm unsure about how to deal with errors here.
                    // For now, allow `maxErrors` to occur before giving up
                    LOG_WARN << "Failed to accept on " << ep << ": " << ec;
                    ++errorCnt;
                    continue;
                }
#ifdef YAHAT_ENABLE_METRICS
                if (internalMetrics()) {
                    internalMetrics()->tcpConnections()->inc();
                }
#endif

                errorCnt = 0;

                if (is_tls) {
                    auto executor = socket.get_executor();
                    boost::asio::spawn(executor, [this, sslCtx, socket=std::move(socket)](boost::asio::yield_context yield) mutable {
                        auto stream = make_shared<beast::ssl_stream<beast::tcp_stream>>(std::move(socket), *sslCtx);
                        try {
                            DoSession<true>(stream, *this, yield);
                        } catch(const exception& ex) {
                            LOG_ERROR << "Caught exception from DoSession [HTTPS]: " << ex.what();
                        }
                    }, boost::asio::detached);

                } else {
                    auto executor = socket.get_executor();
                    boost::asio::spawn(executor, [this, socket=std::move(socket)](boost::asio::yield_context yield) mutable {
                        auto stream = make_shared<beast::tcp_stream>(std::move(socket));
                        try {
                            DoSession<false>(stream, *this, yield);
                        } catch(const exception& ex) {
                            LOG_ERROR << "Caught exception from DoSession [HTTP]: " << ex.what();
                        }
                    }, boost::asio::detached);
                }
            }

        }, boost::asio::detached);
    }; // for resolver endpoint


    startWorkers();
    return promise_.get_future();
}

void HttpServer::run()
{
    LOG_DEBUG << "Starting the HTTP server...";

    ScopedExit when_done{[this] {
        LOG_DEBUG << "The HTTP server is done.";
    }};

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

#ifdef YAHAT_ENABLE_METRICS
Metrics *HttpServer::metrics() noexcept
{
    static Metrics *m{metrics_ ? &metrics_->metrics() : nullptr};
    return m;
}
#endif

#ifdef YAHAT_ENABLE_METRICS
void HttpServer::addRoute_(std::string_view target, handler_t handler, const std::span<std::string_view> methods)
#else
void HttpServer::addRoute(std::string_view target, handler_t handler)
#endif
{
    if (target.size() == 0) {
        throw runtime_error{"A target's route cannot be empty"};
    }
#ifdef YAHAT_ENABLE_METRICS
    if (internalMetrics()) {
        internalMetrics()->addHttpRequests(target, methods);
    }
#endif
    string key{target};
    route_options_.erase(key);
    routes_[std::move(key)] = handler;
}

void HttpServer::addRouteWithOptions(std::string_view target, handler_t handler, RouteOptions options) {
    if (!handler) {
        throw std::invalid_argument{"Route handler cannot be null"};
    }
    addRoute(target, std::move(handler));
    route_options_[std::string{target}] = std::move(options);
}

std::map<std::string, HttpServer::handler_t>::const_iterator
HttpServer::findRoute(std::string_view target) const {
    auto best = routes_.end();
    for (auto it = routes_.begin(); it != routes_.end(); ++it) {
        const auto& route = it->first;
        const auto policy = route_options_.find(route);
        const bool exact = policy != route_options_.end() && policy->second.exact_match;
        if (target == route || (!exact && target.size() > route.size()
            && target.substr(0, route.size()) == route && target[route.size()] == '/')) {
            if (best == routes_.end() || route.size() > best->first.size()) {
                best = it;
            }
        }
    }
    return best;
}

const RouteOptions& HttpServer::routeOptions(std::string_view target) const {
    static const RouteOptions defaults;
    const auto route = findRoute(target);
    if (route != routes_.end()) {
        if (auto it = route_options_.find(route->first); it != route_options_.end()) {
            return it->second;
        }
    }
    return defaults;
}

void Response::applyHeaders(http::fields& target) const {
    for (const auto& header : headers) {
        const auto name = header.name_string();
        const auto value = header.value();
        if (name.empty() || std::any_of(name.begin(), name.end(), [](unsigned char ch) {
            return !((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                || (ch >= '0' && ch <= '9') || std::string_view{"!#$%&'*+-.^_`|~"}.find(ch) != std::string_view::npos);
        }) || std::any_of(value.begin(), value.end(), [](unsigned char ch) {
            return (ch < 32 && ch != '\t') || ch == 127;
        })) {
            throw std::invalid_argument{"Invalid HTTP response header"};
        }
        switch (header.name()) {
        case http::field::content_length:
        case http::field::transfer_encoding:
        case http::field::connection:
        case http::field::keep_alive:
        case http::field::trailer:
        case http::field::upgrade:
            throw std::invalid_argument{"HTTP framing headers are managed by the server"};
        default: break;
        }
    }
    // Replace generated values while preserving repeated application fields.
    for (const auto& header : headers) target.erase(header.name_string());
    for (const auto& header : headers) target.insert(header.name_string(), header.value());
}

std::pair<bool, string_view> HttpServer::Authenticate(const std::string_view &/*authHeader*/)
{
    // TODO: Implement
    static const string_view teste{"teste"};
    return {true, teste};
}

Response HttpServer::onRequest(Request &req) noexcept
{
    const auto match = findRoute(req.target);
    auto* best_handler = match == routes_.end() ? nullptr : match->second.get();
    const std::string_view best_route = match == routes_.end() ? std::string_view{} : match->first;
    const std::string_view tw = req.target;

    if (best_handler) {
        try {
            LOG_TRACE << "Found route '" << best_route << "' for target '" << tw << "'";
            req.route = best_route;
#ifdef YAHAT_ENABLE_METRICS
            auto * metrics = this->internalMetrics();
            if (metrics) {
                metrics->incrementHttpRequestCount(best_route, toString(req.type));
            }

            if (auto *hist = best_handler->metrics()) {
                req.requestDuration = hist->scoped();
            }
#endif
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
#ifdef YAHAT_ENABLE_METRICS
            YahatInstanceMetrics::gauge_scoped_t count_instance;

            if (internalMetrics()) {
                count_instance = internalMetrics()->workerThreads()->scoped();
            }
#endif
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

    filesystem::path raw = root_;

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
    filesystem::path index = path;
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
