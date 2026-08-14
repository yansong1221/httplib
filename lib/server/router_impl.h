#pragma once
#include "httplib/server/router.hpp"
#include "httplib/util/string_hash.hpp"
#include <boost/beast/http/verb.hpp>
#include <functional>
#include <memory>
#include <regex>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace httplib::server
{

    class router_impl : public router
    {
      public:
        router_impl();

        coro_http_handler_type wrap_global(coro_http_handler_type&& handler) const;

        struct Node;

        struct route_match
        {
            std::set<std::string> allows;
            bool chunked = false;
            Node const* node = nullptr;
            std::unordered_map<std::string, std::string> params;
        };

        net::awaitable<void> process_routing(route_match const& match, request& req, response& resp) const;

        struct ws_handler_entry
        {
            websocket_conn::coro_open_handler_type open_handler;
            websocket_conn::coro_close_handler_type close_handler;
            websocket_conn::coro_message_handler_type message_handler;
        };
        std::optional<ws_handler_entry> query_ws_handler(request& req) const;

        std::optional<coro_http_handler_type> query_connect_handler(request& req) const;

        net::awaitable<route_match> pre_routing(request& req) const;
        net::awaitable<void> post_routing(request& req, response& resp) const;

        void reset();

      protected:
        void set_http_handler_impl(http::verb method, std::string_view key, coro_http_handler_type&& handler) override;
        void set_not_found_handler_impl(coro_http_handler_type&& handler) override;
        void set_ws_handler_impl(std::string_view path,
                                 websocket_conn::coro_open_handler_type&& open_handler,
                                 websocket_conn::coro_message_handler_type&& message_handler,
                                 websocket_conn::coro_close_handler_type&& close_handler) override;
        void set_post_routing_handler_impl(coro_http_handler_type&& handler) override;
        void set_connect_handler_impl(std::string_view key, coro_http_handler_type&& handler) override;
        void set_chunked_http_handler_impl(http::verb method,
                                           std::string_view key,
                                           coro_http_handler_type&& handler) override;
        void use_impl(coro_mw_handler_type&& before, coro_mw_handler_type&& after) override;

      private:
        static void collect_allows(std::set<std::string>& allows, Node const* node);

        struct Node
        {
            std::string key;
            std::string param_name;
            std::regex regex;

            std::unordered_map<http::verb, coro_http_handler_type> handlers;
            std::unordered_map<http::verb, coro_http_handler_type> chunked_handlers;
            std::optional<ws_handler_entry> ws_handler;
            std::optional<coro_http_handler_type> connect_handler;

            util::string_map<std::unique_ptr<Node>> static_children;
            std::vector<std::unique_ptr<Node>> param_children;
            std::vector<std::unique_ptr<Node>> regex_children;
            std::unique_ptr<Node> wildcard_children;
        };

        std::unique_ptr<Node> root_;
        mutable std::shared_mutex mutex_;

        coro_http_handler_type post_routing_handler_;
        coro_http_handler_type not_found_handler_;

        std::vector<coro_mw_handler_type> global_before_;
        std::vector<coro_mw_handler_type> global_after_;

        // 内部函数
        static Node* insert(Node* node, std::vector<std::string_view> const& segments, size_t index);

        using MatchHandlerType = std::function<bool(Node const* node)>;

        Node const* match_nodes(Node const* node,
                                std::vector<std::string_view> const& segments,
                                size_t index,
                                std::unordered_map<std::string, std::string>& params,
                                MatchHandlerType const& handler) const;
    };
} // namespace httplib::server