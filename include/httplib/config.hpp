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

} // namespace boost

namespace spdlog
{
    class logger;
}

namespace httplib
{

    // 公共面上刻意没有 boost::beast 的别名：公共 API 不出现 Beast，beast 的别名
    // 在 lib/beast_alias.hpp。Asio 的别名保留，因为 net::awaitable 是本库公开的
    // 协程模型。
    namespace net = boost::asio;
    namespace ssl = boost::asio::ssl;
    using tcp = net::ip::tcp;
    namespace fs = std::filesystem;

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