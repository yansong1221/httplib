#pragma once
#include "httplib/config.hpp"
#include "httplib/router.hpp"

#include "httplib/html.hpp"
#include "httplib/http_handler.hpp"
#include "httplib/server.hpp"
#include "httplib/util/type_traits.h"
#include "radix_tree.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/beast/version.hpp>
#include <functional>
#include <regex>
#include <set>
#include <spdlog/spdlog.h>
#include <tuple>
#include <unordered_map>

namespace httplib {
class router_impl
{
public:
    net::awaitable<bool> handle_file_request(request& req, response& res);
    net::awaitable<void> proc_routing(request& req, response& resp);

    bool set_mount_point(const std::string& mount_point,
                         const std::filesystem::path& dir,
                         const http::fields& headers = {});
    bool remove_mount_point(const std::string& mount_point);

public:
    void set_http_handler_impl(http::verb method,
                               std::string_view key,
                               coro_http_handler_type&& handler);
    void set_default_handler_impl(coro_http_handler_type&& handler);
    void set_file_request_handler_impl(coro_http_handler_type&& handler);

private:
    using verb_handler_map = std::unordered_map<http::verb, coro_http_handler_type>;
    std::unordered_map<std::string, verb_handler_map> coro_handles_;

    std::shared_ptr<radix_tree> coro_router_tree_ = std::make_shared<radix_tree>(radix_tree());
    std::vector<std::tuple<std::regex, coro_http_handler_type>> coro_regex_handles_;

    coro_http_handler_type default_handler_;
    coro_http_handler_type file_request_handler_;

    struct mount_point_entry
    {
        std::string mount_point;
        fs::path base_dir;
        http::fields headers;
    };
    std::vector<mount_point_entry> static_file_entry_;

    std::vector<std::string> default_doc_name_ = {"index.html", "index.htm"};
};
} // namespace httplib