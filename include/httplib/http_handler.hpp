#pragma once
#include "httplib/variant_handler.hpp"

namespace httplib {

class request;
class response;

using coro_http_handler_type = std::function<net::awaitable<void>(request &req, response &resp)>;
using http_handler_type = std::function<void(request &req, response &resp)>;

using http_handler_variant = variant_handler<http_handler_type, coro_http_handler_type>;

} // namespace httplib