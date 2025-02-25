#pragma once
#include <filesystem>

namespace boost
{
namespace asio
{
namespace ip
{
class tcp;
}
namespace ssl
{
}
} // namespace asio

namespace beast
{
namespace http
{
}
namespace websocket
{
}
} // namespace beast

} // namespace boost

namespace spdlog
{
class logger;
}

namespace httplib
{
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace fs = std::filesystem;


} // namespace httplib