#pragma once
#include <boost/asio/awaitable.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http/message.hpp>
#include <regex>

namespace httplib {

namespace beast = boost::beast;   // from <boost/beast.hpp>
namespace http = beast::http;     // from <boost/beast/http.hpp>
namespace net = boost::asio;      // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl; // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>
namespace websocket = beast::websocket;

template<bool isRequest, typename Fields, typename... Bodies>
struct http_message_variant : std::variant<http::message<isRequest, Bodies, Fields>...> {
    using std::variant<http::message<isRequest, Bodies, Fields>...>::variant;

public:
    /// The base class used to hold the header portion of the message.
    using header_type = http::header<isRequest, Fields>;

    const header_type &base() const {
        return std::visit([](auto &t) -> const header_type & { return t.base(); }, *this);
    }

    header_type &base() {
        return std::visit([](auto &t) mutable -> header_type & { return t.base(); }, *this);
    }

    bool chunked() const {
        return std::visit([](auto &t) { return t.chunked(); }, *this);
    }

    void chunked(bool value) {
        std::visit([&](auto &t) mutable { return t.chunked(value); }, *this);
    }

    bool has_content_length() const {
        return std::visit([](auto &t) { return t.has_content_length(); }, *this);
    }

    void content_length(boost::optional<std::uint64_t> const &value) {
        std::visit([&](auto &t) mutable { return t.content_length(value); }, *this);
    }

    bool keep_alive() const {
        return std::visit([](auto &t) { return t.keep_alive(); }, *this);
    }

    void keep_alive(bool value) {
        std::visit([&](auto &t) mutable { return t.keep_alive(value); }, *this);
    }

    bool need_eof() const {
        return std::visit([](auto &t) { return t.need_eof(); }, *this);
    }

    boost::optional<std::uint64_t> payload_size() const {
        return std::visit([](auto &t) { return t.payload_size(); }, *this);
    }

    void prepare_payload() {
        std::visit([](auto &t) mutable { return t.prepare_payload(); }, *this);
    }

    enum class BodyType {
        empty_body,
        string_body,
        file_body,
        dynamic_body
    };

    BodyType body_type() const {
        return std::visit(
            [](auto &t) {
                using body_type = std::decay_t<decltype(t)>::body_type;

                if constexpr (std::same_as<http::empty_body, body_type>)
                    return BodyType::empty_body;
                else if constexpr (std::same_as<http::string_body, body_type>)
                    return BodyType::string_body;
                else if constexpr (std::same_as<http::file_body, body_type>)
                    return BodyType::file_body;
                else if constexpr (std::same_as<http::dynamic_body, body_type>)
                    return BodyType::dynamic_body;
                else
                    static_assert(false, "unknown body");
            },
            *this);
    }

    template<class Body>
    http::message<isRequest, Body> &value() {
        return std::get<http::message<isRequest, Body>>(*this);
    }
    template<class Body>
    const http::message<isRequest, Body> &value() const {
        return std::get<http::message<isRequest, Body>>(*this);
    }

    template<class Body>
    typename Body::value_type &body() & {
        return value<Body>().body();
    }
    template<class Body>
    const typename Body::value_type &body() const & {
        return value<Body>().body();
    }
    template<class Body>
    typename Body::value_type &&body() && {
        return std::move(value<Body>().body());
    }

    template<class Body>
    void change_body() {
        std::visit(
            [this](auto &&t) mutable {
                http::message<isRequest, Body> message(std::move(t));
                *this = std::move(message);
            },
            *this);
    }

    template<class Body>
    void set_body(typename Body::value_type &&data) {

        change_body<Body>();
        body<Body>() = std::move(data);
        //std::visit(
        //    [this, data = std::move(data)](auto &&t) mutable {
        //        http::message<isRequest, Body> message(std::move(t));
        //        message.body() = std::move(data);
        //        *this = std::move(message);
        //    },
        //    *this);
    }

    http::message_generator to_message_generator() {
        return std::visit(
            [](auto &&t) mutable -> http::message_generator {
                http::message_generator msg(std::move(t));
                return std::move(msg);
            },
            *this);
    }
};

using http_request_variant =
    http_message_variant<true, http::fields, http::empty_body, http::string_body, http::file_body>;

using http_response_variant =
    http_message_variant<false, http::fields, http::empty_body, http::string_body, http::file_body>;

struct request : public http_request_variant {
    using http_request_variant::http_request_variant;

public:
    std::unordered_map<std::string, std::string> params;
    std::smatch matches;
};

struct response : public http_response_variant {
    using http_response_variant::http_response_variant;
};

} // namespace httplib