#pragma once

#pragma once
#ifdef HTTPLIB_ENABLED_SSL
#include "ssl_stream.hpp"
#endif
#include "http_variant_stream.hpp"

namespace httplib
{

using http_stream = beast::basic_stream<net::ip::tcp, net::any_io_executor, beast::simple_rate_policy>;
#ifdef HTTPLIB_ENABLED_SSL
using ssl_http_stream = ssl_stream<http_stream>;

using http_variant_stream_type = http_variant_stream<http_stream, ssl_http_stream>;
#else
using http_variant_stream_type = http_variant_stream<http_stream>;
#endif

} // namespace httplib