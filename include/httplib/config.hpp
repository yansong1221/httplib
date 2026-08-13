#pragma once
#include <cstddef>
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

    inline constexpr std::size_t io_buffer_size = 64 * 1024;
    inline constexpr std::size_t file_body_buffer_size = 64 * 1024;

} // namespace httplib
#ifdef HTTPLIB_SHARED_LIBRARY
#if defined(_WIN32)
#if defined(HTTPLIB_EXPORTS)
#define HTTPLIB_API __declspec(dllexport)
#else
#define HTTPLIB_API __declspec(dllimport)
#endif
#else
#define HTTPLIB_API
#endif
#else
#define HTTPLIB_API
#endif