#include "router_impl.h"
#include "request_impl.hpp"
#include "response_impl.hpp"
#include <boost/algorithm/string/join.hpp>
#include <exception>
#include <iostream>
#include <set>

namespace httplib::server
{

    namespace detail
    {

        static auto
        split_segments(std::string_view path)
        {
            auto segments = util::split(path, "/");

            if (path.ends_with("/"))
            {
                segments.push_back(std::string_view());
            }

            return segments;
        }

    } // namespace detail

    router_impl::router_impl() : root_(std::make_unique<Node>()) {}

    void
    router_impl::set_http_handler_impl(http::verb method, std::string_view key, coro_http_handler_type&& handler)
    {
        std::unique_lock lock(mutex_);
        auto segments = detail::split_segments(key);
        auto node = insert(root_.get(), segments, 0);
        node->handlers[method] = wrap_global(std::move(handler));
    }

    router::router::coro_http_handler_type
    router_impl::wrap_global(coro_http_handler_type&& handler) const
    {
        return [handler = std::move(handler), this](request& req, response& resp) -> net::awaitable<void>
        {
            bool ok = true;
            for (auto& before : global_before_)
            {
                if (!co_await before(req, resp))
                {
                    ok = false;
                    break;
                }
            }

            std::exception_ptr eptr;
            if (ok)
            {
                try
                {
                    co_await handler(req, resp);
                }
                catch (...)
                {
                    eptr = std::current_exception();
                }
            }

            // handler 正常返回（或 before 短路）才执行 after；抛异常时跳过，交给外层设 500
            if (!eptr)
            {
                for (auto& after : global_after_)
                {
                    if (!co_await after(req, resp))
                    {
                        break;
                    }
                }
            }

            if (eptr)
            {
                std::rethrow_exception(eptr);
            }
        };
    }

    router_impl::Node*
    router_impl::insert(Node* parent, std::vector<std::string_view> const& segments, size_t index)
    {
        if (index >= segments.size())
        {
            return parent;
        }

        auto const& seg = segments.at(index);

        if (segments.size() - 1 == index && seg == "*")
        {
            if (!parent->wildcard_children)
            {
                auto node = std::make_unique<Node>();
                node->key = seg;
                parent->wildcard_children = std::move(node);
            }
            return insert(parent->wildcard_children.get(), segments, index + 1);
        }

        if (!seg.empty() && seg.starts_with(":"))
        {
            auto iter
                = std::ranges::find_if(parent->param_children, [&](auto const& node) { return node->key == seg; });

            if (iter != parent->param_children.end())
            {
                return insert(iter->get(), segments, index + 1);
            }

            auto node = std::make_unique<Node>();
            node->key = seg;
            node->param_name = seg.substr(1);

            parent->param_children.push_back(std::move(node));
            return insert(parent->param_children.back().get(), segments, index + 1);
        }

        if (!seg.empty() && seg.front() == '{' && seg.back() == '}')
        {
            auto iter
                = std::ranges::find_if(parent->regex_children, [&](auto const& node) { return node->key == seg; });

            if (iter != parent->regex_children.end())
            {
                return insert(iter->get(), segments, index + 1);
            }

            std::string_view inside = seg.substr(1, seg.size() - 2);
            size_t pos = inside.find(':');
            auto key = inside.substr(pos + 1);

            auto node = std::make_unique<Node>();
            node->key = seg;
            node->param_name = inside.substr(0, pos);
            node->regex = std::regex(key.begin(), key.end());

            parent->regex_children.push_back(std::move(node));
            return insert(parent->regex_children.back().get(), segments, index + 1);
        }
        auto [iter, inserted] = parent->static_children.try_emplace(std::string(seg), nullptr);
        if (inserted)
        {
            iter->second = std::make_unique<Node>();
            iter->second->key = seg;
        }
        return insert(iter->second.get(), segments, index + 1);
    }

    // ---------------- 匹配路由 ----------------
    net::awaitable<void>
    router_impl::process_routing(route_match const& match, request& req, response& resp) const
    {
        if (!match.node)
        {
            co_return;
        }

        get_impl(req).set_path_param(std::unordered_map<std::string, std::string>(match.params));

        if (match.lazy)
        {
            auto iter = match.node->lazy_handlers.find(req.method());
            if (iter != match.node->lazy_handlers.end())
            {
                co_await iter->second(req, resp);
            }
        }
        else
        {
            auto iter = match.node->handlers.find(req.method());
            if (iter != match.node->handlers.end())
            {
                co_await iter->second(req, resp);
            }
        }
    }

    void
    router_impl::use_impl(coro_mw_handler_type&& before, coro_mw_handler_type&& after)
    {
        std::unique_lock lock(mutex_);
        global_before_.push_back(std::move(before));
        global_after_.push_back(std::move(after));
    }

    void
    router_impl::set_not_found_handler_impl(coro_http_handler_type&& handler)
    {
        not_found_handler_ = wrap_global(std::move(handler));
    }

    void
    router_impl::set_ws_handler_impl(std::string_view path,
                                     websocket_conn::coro_open_handler_type&& open_handler,
                                     websocket_conn::coro_message_handler_type&& message_handler,
                                     websocket_conn::coro_close_handler_type&& close_handler)
    {
        std::unique_lock lock(mutex_);
        auto segments = detail::split_segments(path);

        auto node = insert(root_.get(), segments, 0);

        ws_handler_entry entry;
        entry.open_handler = std::move(open_handler);
        entry.message_handler = std::move(message_handler);
        entry.close_handler = std::move(close_handler);
        node->ws_handler = std::move(entry);
    }

    std::optional<router_impl::ws_handler_entry>
    router_impl::query_ws_handler(request& req) const
    {
        std::shared_lock lock(mutex_);
        auto segments = detail::split_segments(req.path());

        std::unordered_map<std::string, std::string> params;
        auto node = match_nodes(root_.get(),
                                segments,
                                0,
                                params,
                                [&](Node const* node) { return node->ws_handler.has_value(); });

        if (!node)
        {
            return std::nullopt;
        }

        return node->ws_handler;
    }
    net::awaitable<router_impl::route_match>
    router_impl::pre_routing(request& req) const
    {
        route_match result;

        std::shared_lock lock(mutex_);
        auto segments = detail::split_segments(req.path());

        result.node = match_nodes(root_.get(),
                                  segments,
                                  0,
                                  result.params,
                                  [&](Node const* node)
                                  {
                                      collect_allows(result.allows, node);
                                      if (node->handlers.find(req.method()) != node->handlers.end())
                                      {
                                          return true;
                                      }
                                      if (node->lazy_handlers.find(req.method()) != node->lazy_handlers.end())
                                      {
                                          result.lazy = true;
                                          return true;
                                      }
                                      return false;
                                  });
        co_return result;
    }

    router_impl::Node const*
    router_impl::match_nodes(Node const* parent,
                             std::vector<std::string_view> const& segments,
                             size_t index,
                             std::unordered_map<std::string, std::string>& params,
                             MatchHandlerType const& handler) const
    {
        if (!parent)
        {
            return nullptr;
        }

        if (index == segments.size())
        {
            if (!handler(parent))
            {
                return nullptr;
            }
            return parent;
        }

        auto const& seg = segments[index];

        // 1) static
        {
            auto iter = parent->static_children.find(seg);
            if (iter != parent->static_children.end())
            {
                if (auto node = match_nodes(iter->second.get(), segments, index + 1, params, handler); node)
                {
                    return node;
                }
            }
        }

        // 2) regex
        for (auto& child : parent->regex_children)
        {
            if (std::regex_match(seg.data(), seg.data() + seg.length(), child->regex))
            {
                params[child->param_name] = std::string(seg);
                if (auto node = match_nodes(child.get(), segments, index + 1, params, handler); node)
                {
                    return node;
                }

                params.erase(child->param_name);
            }
        }

        // 3) param
        for (auto& child : parent->param_children)
        {
            params[child->param_name] = std::string(seg);
            if (auto node = match_nodes(child.get(), segments, index + 1, params, handler); node)
            {
                return node;
            }
            params.erase(child->param_name);
        }

        // 4) wildcard
        if (parent->wildcard_children)
        {
            std::string rest;
            for (size_t i = index; i < segments.size(); ++i)
            {
                if (!rest.empty())
                {
                    rest += "/";
                }
                rest += segments[i];
            }
            params["*"] = std::move(rest);
            if (auto node = match_nodes(parent->wildcard_children.get(), segments, segments.size(), params, handler);
                node)
            {
                return node;
            }
            params.erase("*");
        }
        return nullptr;
    }

    net::awaitable<void>
    router_impl::post_routing(request& req, response& resp) const
    {
        if (resp.is_chunked_done())
        {
            co_return;
        }

        if (not_found_handler_ && get_impl(resp).result() == http::status::not_found)
        {
            co_await not_found_handler_(req, resp);
        }

        if (post_routing_handler_)
        {
            co_await post_routing_handler_(req, resp);
        }

        co_return;
    }

    void
    router_impl::reset()
    {
        root_ = std::make_unique<Node>();
        post_routing_handler_ = nullptr;
        not_found_handler_ = nullptr;
        global_before_.clear();
        global_after_.clear();
    }
    void
    router_impl::set_post_routing_handler_impl(coro_http_handler_type&& handler)
    {
        post_routing_handler_ = std::move(handler);
    }

    void
    router_impl::set_connect_handler_impl(std::string_view path, coro_http_handler_type&& handler)
    {
        std::unique_lock lock(mutex_);
        auto segments = detail::split_segments(path);
        auto node = insert(root_.get(), segments, 0);
        node->connect_handler = wrap_global(std::move(handler));
    }

    std::optional<router::coro_http_handler_type>
    router_impl::query_connect_handler(request& req) const
    {
        std::shared_lock lock(mutex_);
        auto segments = detail::split_segments(req.path());

        std::unordered_map<std::string, std::string> params;
        auto node = match_nodes(root_.get(),
                                segments,
                                0,
                                params,
                                [&](Node const* node) { return node->connect_handler.has_value(); });

        if (!node)
        {
            return std::nullopt;
        }

        return node->connect_handler;
    }

    void
    router_impl::set_lazy_http_handler_impl(http::verb method,
                                            std::string_view key,
                                            coro_http_handler_type&& handler)
    {
        std::unique_lock lock(mutex_);
        auto segments = detail::split_segments(key);
        auto node = insert(root_.get(), segments, 0);
        node->lazy_handlers[method] = wrap_global(std::move(handler));
    }

    void
    router_impl::collect_allows(std::set<std::string>& allows, Node const* node)
    {
        for (auto const& v : node->handlers)
        {
            allows.insert(to_string(v.first));
        }
        for (auto const& v : node->lazy_handlers)
        {
            allows.insert(to_string(v.first));
        }
        if (node->connect_handler)
        {
            allows.insert(to_string(http::verb::connect));
        }
        if (node->ws_handler)
        {
            allows.insert(to_string(http::verb::get));
        }
    }

} // namespace httplib::server
