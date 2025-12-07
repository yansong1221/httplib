#pragma once
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/router.hpp"
#include <boost/beast/http.hpp>
#include <functional>
#include <memory>
#include <regex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace httplib::server {

class router_impl : public router
{
public:
    router_impl();

    net::awaitable<void> proc_routing(request& req, response& resp) const;

    struct ws_handler_entry
    {
        websocket_conn::coro_open_handler_type open_handler;
        websocket_conn::coro_close_handler_type close_handler;
        websocket_conn::coro_message_handler_type message_handler;
    };
    std::optional<ws_handler_entry> query_ws_handler(request& req) const;

    bool pre_routing(request& req, response& resp) const;

protected:
    void set_http_handler_impl(http::verb method,
                               std::string_view path,
                               coro_http_handler_type&& handler) override;
    void set_default_handler_impl(coro_http_handler_type&& handler) override;
    void set_ws_handler_impl(std::string_view path,
                             websocket_conn::coro_open_handler_type&& open_handler,
                             websocket_conn::coro_message_handler_type&& message_handler,
                             websocket_conn::coro_close_handler_type&& close_handler) override;

private:
    struct Node
    {
        std::string key; // Radix key (静态路径段)
        bool is_param    = false;
        bool is_regex    = false;
        bool is_wildcard = false;

        std::string param_name;
        std::regex regex;

        std::unordered_map<http::verb, coro_http_handler_type> handlers;
        std::optional<ws_handler_entry> ws_handler;

        std::vector<std::unique_ptr<Node>> children;
    };

    std::unique_ptr<Node> root_;
    // mutable std::shared_mutex mutex_;

    coro_http_handler_type default_handler_;

    // 内部函数
    std::unique_ptr<Node> make_special_node(std::string_view segment);

    Node* insert(Node* node, const std::vector<std::string_view>& segments, size_t index);

    const Node* match_node(const Node* node,
                           const std::vector<std::string_view>& segments,
                           size_t index,
                           std::unordered_map<std::string, std::string>& path_params) const;
};
} // namespace httplib::server