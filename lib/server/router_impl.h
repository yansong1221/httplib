#pragma once
#include "httplib/server/router.hpp"
#include <boost/beast/http/verb.hpp>
#include <functional>
#include <memory>
#include <regex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace httplib::server {

class request;
class response;

class router_impl : public router
{
public:
    router_impl();

    struct Node;
    enum class body_kind
    {
        none,
        chunked,
        buffer_body,
    };

    struct route_match
    {
        std::set<std::string> allows;
        body_kind body = body_kind::none;
        const Node* node = nullptr;
        std::unordered_map<std::string, std::string> params;
    };

    net::awaitable<void> process_routing(const route_match& match, request& req, response& resp) const;

    struct ws_handler_entry
    {
        websocket_conn::coro_open_handler_type open_handler;
        websocket_conn::coro_close_handler_type close_handler;
        websocket_conn::coro_message_handler_type message_handler;
    };
    std::optional<ws_handler_entry> query_ws_handler(request& req) const;

    net::awaitable<route_match> pre_routing(request& req) const;
    net::awaitable<void> post_routing(request& req, response& resp) const;

protected:
    void set_http_handler_impl(http::verb method,
                               std::string_view key,
                               coro_http_handler_type&& handler) override;
    void set_not_found_handler_impl(coro_http_handler_type&& handler) override;
    void set_ws_handler_impl(std::string_view path,
                             websocket_conn::coro_open_handler_type&& open_handler,
                             websocket_conn::coro_message_handler_type&& message_handler,
                             websocket_conn::coro_close_handler_type&& close_handler) override;
    void set_post_routing_handler_impl(coro_http_handler_type&& handler) override;
    void set_chunked_http_handler_impl(http::verb method,
                                       std::string_view key,
                                       coro_http_handler_type&& handler) override;
    void set_buffer_body_http_handler_impl(http::verb method,
                                      std::string_view key,
                                      coro_http_handler_type&& handler) override;

public:
    static void write_error(response& resp, const std::set<std::string>& allows);

private:
    static void collect_allows(std::set<std::string>& allows,
                               const Node* node,
                               bool include_chunked);

    struct Node
    {
        std::string key;
        std::string param_name;
        std::regex regex;

        std::unordered_map<http::verb, coro_http_handler_type> handlers;
        std::unordered_map<http::verb, coro_http_handler_type> chunked_handlers;
        std::unordered_map<http::verb, coro_http_handler_type> buffer_body_handlers;
        std::optional<ws_handler_entry> ws_handler;

        std::unordered_map<std::string, std::unique_ptr<Node>> static_children;
        std::vector<std::unique_ptr<Node>> param_children;
        std::vector<std::unique_ptr<Node>> regex_children;
        std::unique_ptr<Node> wildcard_children;
    };

    std::unique_ptr<Node> root_;
    mutable std::shared_mutex mutex_;

    coro_http_handler_type post_routing_handler_;
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