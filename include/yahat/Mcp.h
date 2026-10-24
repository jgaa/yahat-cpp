#pragma once

#include "yahat/JsonRpc.h"

#ifdef YAHAT_ENABLE_MCP
#include <boost/asio/awaitable.hpp>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>
#include <type_traits>

namespace yahat {

/// Explicit, safe-to-disclose tool failure. Unexpected exceptions stay private.
class ToolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ArgumentError : public ToolError {
public:
    ArgumentError(std::string path, std::string expected, const boost::json::value* actual);
};

/// Owned invocation state; copies share cooperative cancellation and progress.
/// Progress is marshalled to executor(); await it before the handler returns.
class ToolContext {
public:
    ToolContext(boost::asio::any_io_executor executor, Auth auth = {},
                std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false));
    const Auth& auth() const noexcept { return auth_; }
    auto executor() const { return executor_; }
    bool isCancelled() const noexcept { return cancelled_->load(std::memory_order_relaxed); }
    void throwIfCancelled() const;
    boost::asio::awaitable<void> progress(double completed, std::optional<double> total = {}, std::string message = {}) const;
    boost::json::value request_id;
    std::string correlation_id;
    boost::json::object metadata;
private:
    friend class McpServer;
    boost::asio::any_io_executor executor_;
    Auth auth_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::function<boost::asio::awaitable<void>(boost::json::object)> progress_;
};

/// Escape hatch for MCP content blocks. Ordinary returns become structuredContent.
struct ToolResult {
    boost::json::array content;
    std::optional<boost::json::value> structured_content;
    bool is_error = false;
    static ToolResult fromValue(boost::json::value value);
};

namespace mcp_detail {
template<class> inline constexpr bool unsupported = false;
template<class T> struct Optional : std::false_type {};
template<class T> struct Optional<std::optional<T>> : std::true_type { using element = T; };
template<class T> struct Vector : std::false_type {};
template<class T, class A> struct Vector<std::vector<T, A>> : std::true_type { using element = T; };
template<class T> struct Map : std::false_type {};
template<class V, class C, class A> struct Map<std::map<std::string, V, C, A>> : std::true_type { using element = V; };
std::string pointerToken(std::string_view text);
}

/// Specialize for domain arguments: provide decode(value,path) and schema().
/// Return conversion uses Boost.JSON value_from / tag_invoke customization.
template<class T> struct McpConverter {
    static T decode(const boost::json::value& value, const std::string& path) {
        using namespace mcp_detail;
        if constexpr (std::is_same_v<T, boost::json::value>) return value;
        else if constexpr (std::is_same_v<T, boost::json::object>) {
            if (value.is_object()) return value.as_object();
        } else if constexpr (std::is_same_v<T, boost::json::array>) {
            if (value.is_array()) return value.as_array();
        } else if constexpr (std::is_same_v<T, bool>) {
            if (value.is_bool()) return value.as_bool();
        } else if constexpr (std::is_integral_v<T>) {
            if (value.is_int64() && std::in_range<T>(value.as_int64())) return static_cast<T>(value.as_int64());
            if (value.is_uint64() && std::in_range<T>(value.as_uint64())) return static_cast<T>(value.as_uint64());
        } else if constexpr (std::is_floating_point_v<T>) {
            if (value.is_number()) {
                const auto number = value.is_double() ? value.as_double()
                    : value.is_int64() ? static_cast<double>(value.as_int64()) : static_cast<double>(value.as_uint64());
                if (std::isfinite(number) && std::abs(number) <= std::numeric_limits<T>::max()) return static_cast<T>(number);
            }
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (value.is_string()) return std::string{value.as_string()};
        } else if constexpr (Optional<T>::value) {
            if (value.is_null()) return {};
            return McpConverter<typename Optional<T>::element>::decode(value, path);
        } else if constexpr (Vector<T>::value) {
            if (value.is_array()) {
                T result;
                result.reserve(value.as_array().size());
                size_t index = 0;
                for (const auto& item : value.as_array()) {
                    result.push_back(McpConverter<typename Vector<T>::element>::decode(item, path + "/" + std::to_string(index++)));
                }
                return result;
            }
        } else if constexpr (Map<T>::value) {
            if (value.is_object()) {
                T result;
                for (const auto& item : value.as_object()) {
                    result.emplace(std::string{item.key()}, McpConverter<typename Map<T>::element>::decode(item.value(), path + "/" + pointerToken(item.key())));
                }
                return result;
            }
        } else static_assert(unsupported<T>, "Provide a McpConverter specialization for this argument type");
        throw ArgumentError{path, boost::json::serialize(schema()), &value};
    }

    static boost::json::object schema() {
        using namespace mcp_detail;
        if constexpr (std::is_same_v<T, boost::json::value>) return {};
        else if constexpr (std::is_same_v<T, boost::json::object>) return {{"type", "object"}};
        else if constexpr (std::is_same_v<T, boost::json::array>) return {{"type", "array"}};
        else if constexpr (std::is_same_v<T, bool>) return {{"type", "boolean"}};
        else if constexpr (std::is_integral_v<T>) return {{"type", "integer"}, {"minimum", std::numeric_limits<T>::lowest()}, {"maximum", std::numeric_limits<T>::max()}};
        else if constexpr (std::is_floating_point_v<T>) return {{"type", "number"}};
        else if constexpr (std::is_same_v<T, std::string>) return {{"type", "string"}};
        else if constexpr (Optional<T>::value) return {{"anyOf", boost::json::array{McpConverter<typename Optional<T>::element>::schema(), boost::json::object{{"type", "null"}}}}};
        else if constexpr (Vector<T>::value) return {{"type", "array"}, {"items", McpConverter<typename Vector<T>::element>::schema()}};
        else if constexpr (Map<T>::value) return {{"type", "object"}, {"additionalProperties", McpConverter<typename Map<T>::element>::schema()}};
        else static_assert(unsupported<T>, "Provide a McpConverter specialization for this argument type");
    }
};

struct ToolArgument {
    ToolArgument(const char* name) : name{name} {}
    ToolArgument(std::string name, std::string description = {}) : name{std::move(name)}, description{std::move(description)} {}
    std::string name;
    std::string description;
};

struct ToolDescriptor {
    std::string name;
    std::string description;
    boost::json::object input_schema;
    boost::json::object annotations;
    /// Topic names are explicit keys, never interpreted as paths by yahat.
    std::map<std::string, std::string, std::less<>> help;
    /// Optional lazy provider for known topics (e.g. application embedded files).
    std::function<std::string(std::string_view)> help_provider;
    std::function<boost::asio::awaitable<ToolResult>(boost::json::object, ToolContext)> invoke;
    /// Called for every invocation, before conversion/handler execution. Throw ToolError to deny.
    std::function<void(const ToolContext&)> authorize;
};

namespace mcp_detail {
template<class> struct Awaitable;
template<class T, class E> struct Awaitable<boost::asio::awaitable<T, E>> { using result = T; };
template<class> struct Function;
template<class R, class... A> struct Function<R(A...)> { using result = typename Awaitable<R>::result; using args = std::tuple<A...>; };
template<class R, class... A> struct Function<R(*)(A...)> : Function<R(A...)> {};
template<class R, class... A> struct Function<R(*)(A...) noexcept> : Function<R(A...)> {};
template<class C, class R, class... A> struct Function<R(C::*)(A...)> : Function<R(A...)> {};
template<class C, class R, class... A> struct Function<R(C::*)(A...) const> : Function<R(A...)> {};
template<class C, class R, class... A> struct Function<R(C::*)(A...) noexcept> : Function<R(A...)> {};
template<class C, class R, class... A> struct Function<R(C::*)(A...) const noexcept> : Function<R(A...)> {};
template<class F> struct Function : Function<decltype(&F::operator())> {};

template<class Tuple> constexpr bool hasContext() {
    if constexpr (std::tuple_size_v<Tuple> == 0) return false;
    else return std::is_same_v<std::remove_cvref_t<std::tuple_element_t<0, Tuple>>, ToolContext>;
}

template<class T> auto argument(const boost::json::object& values, const std::string& name) {
    using V = std::remove_cvref_t<T>;
    static_assert(!std::is_reference_v<T> || std::is_const_v<std::remove_reference_t<T>>, "Tool parameters must be values or const references");
    const auto* value = values.if_contains(name);
    const auto path = "/" + pointerToken(name);
    if (!value) {
        if constexpr (Optional<V>::value) return V{};
        else throw ArgumentError{path, "required argument", nullptr};
    }
    return McpConverter<V>::decode(*value, path);
}

template<class Traits, class F, size_t... I>
ToolDescriptor make(std::string name, std::string description, std::vector<ToolArgument> arguments, F fn, std::index_sequence<I...>) {
    using Tuple = typename Traits::args;
    constexpr size_t offset = hasContext<Tuple>() ? 1 : 0;
    if (arguments.size() != sizeof...(I)) throw std::invalid_argument{"Tool argument names do not match handler signature"};
    boost::json::object properties;
    boost::json::array required;
    auto add = [&]<size_t N>() {
        using T = std::remove_cvref_t<std::tuple_element_t<N + offset, Tuple>>;
        const auto& arg = arguments[N];
        if (arg.name.empty() || properties.contains(arg.name)) throw std::invalid_argument{"Empty or duplicate tool argument name"};
        auto schema = McpConverter<T>::schema();
        if (!arg.description.empty()) schema["description"] = arg.description;
        properties[arg.name] = std::move(schema);
        if constexpr (!Optional<T>::value) required.push_back(boost::json::string{arg.name});
    };
    (add.template operator()<I>(), ...);
    ToolDescriptor tool;
    tool.name = std::move(name);
    tool.description = std::move(description);
    tool.input_schema = {{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}};
    tool.invoke = [fn = std::move(fn), arguments = std::move(arguments)](boost::json::object values, ToolContext context) -> boost::asio::awaitable<ToolResult> {
        for (const auto& entry : values) {
            if (std::none_of(arguments.begin(), arguments.end(), [&](const auto& arg) { return arg.name == entry.key(); })) {
                throw ArgumentError{"/" + pointerToken(entry.key()), "declared argument name", &entry.value()};
            }
        }
        // Materialize owned parameters before entering the application coroutine.
        auto converted = std::tuple<std::remove_cvref_t<std::tuple_element_t<I + offset, Tuple>>...>{argument<std::tuple_element_t<I + offset, Tuple>>(values, arguments[I].name)...};
        auto call = [&]() {
            if constexpr (offset) return std::apply([&](auto&... args) { return std::invoke(fn, context, std::move(args)...); }, converted);
            else return std::apply([&](auto&... args) { return std::invoke(fn, std::move(args)...); }, converted);
        };
        context.throwIfCancelled();
        using R = typename Traits::result;
        if constexpr (std::is_void_v<R>) {
            co_await call();
            co_return ToolResult{};
        } else if constexpr (std::is_same_v<R, ToolResult>) co_return co_await call();
        else co_return ToolResult::fromValue(boost::json::value_from(co_await call()));
    };
    return tool;
}
}

template<class F>
ToolDescriptor makeTool(std::string name, std::string description, std::vector<ToolArgument> arguments, F handler) {
    using Traits = mcp_detail::Function<F>;
    constexpr size_t count = std::tuple_size_v<typename Traits::args> - (mcp_detail::hasContext<typename Traits::args>() ? 1 : 0);
    return mcp_detail::make<Traits>(std::move(name), std::move(description), std::move(arguments), std::move(handler), std::make_index_sequence<count>{});
}

/// The instance must outlive the registry and all outstanding invocations.
template<class C, class M>
ToolDescriptor makeTool(std::string name, std::string description, std::vector<ToolArgument> arguments, C& instance, M method) {
    using Traits = mcp_detail::Function<M>;
    constexpr size_t count = std::tuple_size_v<typename Traits::args> - (mcp_detail::hasContext<typename Traits::args>() ? 1 : 0);
    auto bound = [&instance, method](auto&&... args) { return std::invoke(method, instance, std::forward<decltype(args)>(args)...); };
    return mcp_detail::make<Traits>(std::move(name), std::move(description), std::move(arguments), std::move(bound), std::make_index_sequence<count>{});
}

class McpServer : public std::enable_shared_from_this<McpServer> {
public:
    static constexpr std::string_view protocol_version = "2026-07-28";
    struct Identity { std::string name; std::string version; std::string instructions; };
    struct Options {
        bool enable_help = true;
        size_t page_size = 64;
        size_t max_input_bytes = 1024 * 1024;
        size_t max_output_bytes = 1024 * 1024;
        size_t max_help_bytes = 32 * 1024;
        size_t max_concurrent_calls = 64;
        /// Global fixed window; applications may add principal-specific limits in authorize.
        size_t calls_per_minute = 600;
        uint64_t catalog_ttl_ms = 300000;
    };
    explicit McpServer(Identity identity);
    McpServer(Identity identity, Options options);
    void add(ToolDescriptor tool);
    void freeze();
    /// Mounts an exact POST route; retains this shared-owned server. Authentication
    /// is inherited. Present Origins are denied unless explicitly allowed.
    void mount(HttpServer& http, std::string path = "/mcp", RouteOptions options = {});
    /// Transport-independent execution; message/context are owned across suspension.
    boost::asio::awaitable<boost::json::object> dispatch(boost::json::value message, ToolContext context) const;
private:
    class Handler;
    Identity identity_;
    Options options_;
    std::map<std::string, ToolDescriptor, std::less<>> tools_;
    std::atomic_bool frozen_{false};
    mutable std::mutex limits_mutex_;
    mutable size_t active_calls_ = 0;
    mutable size_t window_calls_ = 0;
    mutable std::chrono::steady_clock::time_point window_start_ = std::chrono::steady_clock::now();
    std::string catalog_id_;
    boost::json::object result(boost::json::value id, boost::json::object body) const;
    boost::json::object help(std::optional<std::string> name, std::optional<std::string> topic, const ToolContext& context) const;
};

} // namespace yahat
#endif
