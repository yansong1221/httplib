#pragma once
#include <cstddef>
#include <filesystem>

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

namespace boost
{
    namespace asio
    {
        class io_context;
        namespace ip
        {
            class tcp;
        }
        namespace ssl
        {
        }
    } // namespace asio

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
    namespace fs = std::filesystem;

} // namespace httplib
