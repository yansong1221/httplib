#pragma once
#include <utility>
#include <vector>

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
namespace std::filesystem
{

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

using range_type = std::pair<int64_t, int64_t>;
using http_ranges = std::vector<range_type>;

} // namespace httplib