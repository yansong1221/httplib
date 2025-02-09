#pragma once
#include <boost/beast.hpp>

namespace httplib {

namespace beast = boost::beast;   // from <boost/beast.hpp>
namespace http = beast::http;     // from <boost/beast/http.hpp>
namespace net = boost::asio;      // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl; // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>
namespace websocket = beast::websocket;

enum class BodyType {
    empty_body,
    string_body,
    file_body,
    dynamic_body
};

namespace detail {

template<bool isRequest, typename... Bodies>
struct http_message_variant : std::variant<http::message<isRequest, Bodies>...> {
    using std::variant<http::message<isRequest, Bodies>...>::variant;
};

template<bool isRequest>
using http_message_variant_type =
    http_message_variant<isRequest, http::empty_body, http::string_body, http::file_body,
                         http::dynamic_body>;

template<bool isRequest>
class message_variant : public http_message_variant_type<isRequest> {
    using http_message_variant_type<isRequest>::http_message_variant;

public:
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
                    static_assert(false, "unknown body type");
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
};

} // namespace detail

struct request : public detail::message_variant<true> {
    using detail::message_variant<true>::message_variant;
};

struct response : public detail::message_variant<false> {
    using detail::message_variant<false>::message_variant;
};

} // namespace httplib