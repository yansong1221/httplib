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

    net::awaitable<bool> pre_routing(request& req, response& resp) const;
    net::awaitable<void> post_routing(request& req, response& resp) const;

protected:
    void set_http_handler_impl(http::verb method,
                               std::string_view path,
                               coro_http_handler_type&& handler) override;
    void set_not_found_handler_impl(coro_http_handler_type&& handler) override;
    void set_ws_handler_impl(std::string_view path,
                             websocket_conn::coro_open_handler_type&& open_handler,
                             websocket_conn::coro_message_handler_type&& message_handler,
                             websocket_conn::coro_close_handler_type&& close_handler) override;
    void set_http_post_handler_impl(coro_http_handler_type&& handler) override;

private:
    struct Node
    {
        enum class node_type
        {
            static_node,
            param_node,
            regex_node,
            wildcard_node,
        };

        std::string key; // Radix key (静态路径段)
        std::string param_name;
        std::regex regex;
        node_type type = node_type::static_node;

        std::unordered_map<http::verb, coro_http_handler_type> handlers;
        std::optional<ws_handler_entry> ws_handler;

        std::unordered_map<std::string, std::unique_ptr<Node>> static_children;
        std::vector<std::unique_ptr<Node>> param_children;
        std::vector<std::unique_ptr<Node>> regex_children;
        std::unique_ptr<Node> wildcard_children;
    };

    std::unique_ptr<Node> root_;
    // mutable std::shared_mutex mutex_;

    coro_http_handler_type post_handler_;
    coro_http_handler_type not_found_handler_;

    // 内部函数
    static Node* insert(Node* node, const std::vector<std::string_view>& segments, size_t index);

    using MatchHandlerType = std::function<bool(const Node* node)>;

    const Node* match_nodes(const Node* node,
                            const std::vector<std::string_view>& segments,
                            size_t index,
                            std::unordered_map<std::string, std::string>& params,
                            const MatchHandlerType& handler) const;
};
} // namespace httplib::server