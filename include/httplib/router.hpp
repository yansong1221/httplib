#pragma once
#include "httplib/http_handler.hpp"
#include "httplib/util/type_traits.h"
#include "httplib/variant_message.hpp"
#include <algorithm>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <filesystem>
#include <list>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

namespace httplib {

class radix_tree;
class router {
public:
    struct mount_point_entry {
        std::string mount_point;
        std::filesystem::path base_dir;
        http::fields headers;
    };

    router(std::shared_ptr<spdlog::logger> logger);

public:
    // eg: "GET hello/" as a key
    template<typename Func, typename... Aspects>
    void set_http_handler(http::verb method, std::string_view key, Func &&handler,
                          Aspects &&...asps);

    template<http::verb... method, typename Func, typename... Aspects>
    void set_http_handler(std::string_view key, Func &&handler, Aspects &&...asps) {
        static_assert(sizeof...(method) >= 1, "must set http_method");
        (set_http_handler(method, key, handler, std::forward<Aspects>(asps)...), ...);
    }
    template<http::verb... method, typename Func, typename... Aspects>
    void set_http_handler(std::string_view key, Func &&handler, util::class_type_t<Func> &owner,
                          Aspects &&...asps);

    template<typename Func, typename... Aspects>
    void set_default_handler(Func &&handler, Aspects &&...asps);

    template<typename Func, typename... Aspects>
    void set_file_request_handler(Func &&handler, Aspects &&...asps);

    inline bool set_mount_point(const std::string &mount_point, const std::filesystem::path &dir,
                                const http::fields &headers = {});

    inline bool remove_mount_point(const std::string &mount_point);

    inline bool has_handler(http::verb method, std::string_view target) const;

    inline net::awaitable<void> routing(request &req, response &resp);

private:
    inline net::awaitable<bool> handle_file_request(request &req, response &res);
    inline net::awaitable<void> proc_routing_befor(request &req, response &resp);
    inline net::awaitable<void> proc_routing(request &req, response &resp);
    inline net::awaitable<void> proc_routing_after(request &req, response &resp);

private:
    std::shared_ptr<spdlog::logger> logger_;

    using verb_handler_map = std::unordered_map<http::verb, coro_http_handler_type>;
    std::unordered_map<std::string, verb_handler_map> coro_handles_;

    std::shared_ptr<radix_tree> coro_router_tree_;
    std::vector<std::tuple<std::regex, coro_http_handler_type>> coro_regex_handles_;

    coro_http_handler_type default_handler_;
    coro_http_handler_type file_request_handler_;

    std::vector<mount_point_entry> static_file_entry_;
};

bool router::has_handler(http::verb method, std::string_view target) const {
    return true;
}

} // namespace httplib

#include "httplib/impl/router.hpp"