#pragma once
#include "httplib/body/body.hpp"

#include "httplib/mime_types.hpp"
#include "httplib/use_awaitable.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/message.hpp>
#include <filesystem>
#include <regex>

namespace httplib {

template<bool isRequest, typename Fields, typename... Bodies>
struct variant_message : std::variant<http::message<isRequest, Bodies, Fields>...> {
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
    header_type &operator->() {
        return base();
    }
    const header_type &operator->() const {
        return base();
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

    template<class Body>
    bool is_body_type() const {
        return std::visit(
            [](auto &t) {
                using body_type = std::decay_t<decltype(t)>::body_type;
                if constexpr (std::same_as<Body, body_type>)
                    return true;
                else
                    return false;
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
    typename Body::value_type &change_body() & {
        if (!is_body_type<Body>()) {
            std::visit(
                [this](auto &&t) mutable {
                    http::message<isRequest, Body> message(std::move(t));
                    *this = std::move(message);
                },
                *this);
        }
        return body<Body>();
    }

    template<class Body>
    void set_body(typename Body::value_type &&data) {
        change_body<Body>();
        body<Body>() = std::move(data);
    }
    template<class Body>
    void set_body(const typename Body::value_type &data) {
        change_body<Body>();
        body<Body>() = data;
    }
};

template<bool isRequest, typename Fields = http::fields>
using http_message_variant =
    variant_message<isRequest, Fields, body::empty_body, body::string_body, body::file_body,
                    body::form_data_body, body::json_body>;

using http_request_variant = http_message_variant<true>;
using http_response_variant = http_message_variant<false>;

} // namespace httplib