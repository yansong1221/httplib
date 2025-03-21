#pragma once
#include "httplib/server.hpp"
#include "httplib/http_handler.hpp"
#include <algorithm>
#include <boost/asio/detached.hpp>
#include <boost/beast/http/fields.hpp>
#include <filesystem>
#include <list>
#include <string>
#include <string_view>

namespace httplib {
class router
{
  public:
    explicit router(const server::setting& option);
    virtual ~router();

  public:
    // eg: "GET hello/" as a key
    template<typename Func, typename... Aspects>
    void
    set_http_handler(http::verb method,
                     std::string_view key,
                     Func&& handler,
                     Aspects&&... asps);

    template<http::verb... method, typename Func, typename... Aspects>
    void
    set_http_handler(std::string_view key, Func&& handler, Aspects&&... asps)
    {
        static_assert(sizeof...(method) >= 1, "must set http_method");
        (set_http_handler(method, key, handler, std::forward<Aspects>(asps)...), ...);
    }
    template<http::verb... method, typename Func, typename... Aspects>
    void
    set_http_handler(std::string_view key,
                     Func&& handler,
                     util::class_type_t<Func>& owner,
                     Aspects&&... asps);

    template<typename Func, typename... Aspects>
    void
    set_default_handler(Func&& handler, Aspects&&... asps);

    template<typename Func, typename... Aspects>
    void
    set_file_request_handler(Func&& handler, Aspects&&... asps);

    bool
    set_mount_point(const std::string& mount_point,
                    const std::filesystem::path& dir,
                    const http::fields& headers = {});

    bool
    remove_mount_point(const std::string& mount_point);

    bool
    has_handler(http::verb method, std::string_view target) const;

    net::awaitable<void>
    routing(request& req, response& resp);

  private:
    void
    set_http_handler_impl(http::verb method,
                          std::string_view key,
                          coro_http_handler_type&& handler);
    void
    set_default_handler_impl(coro_http_handler_type&& handler);
    void
    set_file_request_handler_impl(coro_http_handler_type&& handler);

  private:
    class impl;
    std::unique_ptr<impl> impl_;
};

} // namespace httplib

#include "httplib/router.inl"