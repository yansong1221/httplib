#pragma once
#ifdef HTTLIP_ENABLED_SSL
#include "ssl_stream.hpp"
#endif
#include "http_stream.hpp"
#include "websocket_variant_stream.hpp"

namespace httplib {

using websocket_stream = websocket::stream<http_stream>;
#ifdef HTTLIP_ENABLED_SSL
using ssl_websocket_stream = websocket::stream<ssl_http_stream>;
using websocket_variant_stream_type =
    websocket_variant_stream<websocket_stream, ssl_websocket_stream>;
#else
using websocket_variant_stream_type = websocket_variant_stream<websocket_stream>;
#endif

inline static websocket_variant_stream_type
create_websocket_variant_stream(http_variant_stream_type &&stream) {
    return std::visit(
        [](auto &&t) -> websocket_variant_stream_type {
            using value_type = std::decay_t<decltype(t)>;
            
            if constexpr (std::same_as<http_stream, value_type>) {
                return websocket_stream(std::move(t));
            }
#ifdef HTTLIP_ENABLED_SSL
            else if constexpr (std::same_as<ssl_http_stream, value_type>) {
                return ssl_websocket_stream(std::move(t));
            }
#endif
        },
        stream);
}

} // namespace httplib