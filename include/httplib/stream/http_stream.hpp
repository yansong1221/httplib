#pragma once
#include "ssl_stream.hpp"
#include "variant_stream.hpp"

namespace httplib::stream {

template<typename... T>
class http_stream_variant : public variant_stream<T...> {
public:
    using variant_stream<T...>::variant_stream;

public:
    auto expires_after(const net::steady_timer::duration &expiry_time) {
        return std::visit(
            [&](auto &t) mutable { return beast::get_lowest_layer(t).expires_after(expiry_time); },
            *this);
    }
    auto expires_never() {
        return std::visit(
            [&](auto &t) mutable { return beast::get_lowest_layer(t).expires_never(); }, *this);
    }

    auto rate_policy() noexcept {
        return std::visit([&](auto &t) mutable { return beast::get_lowest_layer(t).rate_policy(); },
                          *this);
    }
    auto rate_policy() const noexcept {
        return std::visit([&](auto &t) { return beast::get_lowest_layer(t).rate_policy(); }, *this);
    }
};

using http_stream =
    beast::basic_stream<net::ip::tcp, net::any_io_executor, beast::simple_rate_policy>;
using ssl_http_stream = ssl_stream<http_stream>;

using http_stream_variant_type = http_stream_variant<http_stream, ssl_http_stream>;

} // namespace httplib::stream