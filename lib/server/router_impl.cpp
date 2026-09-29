#include "router_impl.h"
#include "beast_alias.hpp"
#include "enum_conv.hpp"
#include "request_impl.hpp"
#include "response_impl.hpp"
#include <boost/algorithm/string/join.hpp>
#include <cstddef>
#include <exception>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace httplib::server
{

    namespace detail
    {

        /// 单条路由正则的长度上限。超长 pattern 既是编译期开销，也是匹配期开销。
        constexpr std::size_t regex_pattern_max = 256;

        /// 参与正则匹配的路径段长度上限。超长段直接判定为不匹配，
        /// 避免病态 pattern 在超长输入上做无意义的回溯。
        constexpr std::size_t regex_subject_max = 1024;

        /// 路由正则走 `std::regex`（回溯实现，非 RE2），而**被匹配的路径段完全由
        /// 请求方控制**。因此在注册阶段做一次准入检查，把「灾难性回溯」从请求期
        /// 挂死执行线程前移成启动期报错。
        ///
        /// 拒绝的结构（经典 ReDoS 形态）：
        /// - 量词嵌套：`(a+)+`、`(a*)*`、`(a{2,})+` …；
        /// - 被量词作用且内部含交替的分组：`(a|aa)+` …。
        ///
        /// 这是一层启发式防线，不等价于线性时间保证；未覆盖的形态（例如不带分组的
        /// 多重无界量词 `a*a*a*b`）由 regex_subject_max 限制最坏输入规模来兜底。
        bool
        has_redos_shape(std::string_view pattern)
        {
            // 每个分组内是否已出现「量词」或「交替」。
            std::vector<bool> groups;
            bool in_class = false;

            for (std::size_t i = 0; i < pattern.size(); ++i)
            {
                auto c = pattern[i];

                if (c == '\\')
                {
                    ++i; // 跳过被转义的字符
                    continue;
                }
                if (in_class)
                {
                    if (c == ']')
                    {
                        in_class = false;
                    }
                    continue;
                }

                switch (c)
                {
                    case '[':
                        in_class = true;
                        break;
                    case '(':
                        groups.push_back(false);
                        break;
                    case ')':
                    {
                        if (groups.empty())
                        {
                            // 括号不配对，交给 std::regex 报 std::regex_error。
                            return false;
                        }
                        // 必须显式取 bool：vector<bool>::back() 返回代理引用，
                        // 用 auto 承接会得到一个指向内部存储的悬垂代理。
                        bool const risky = groups.back();
                        groups.pop_back();
                        if (risky && i + 1 < pattern.size())
                        {
                            auto q = pattern[i + 1];
                            if (q == '*' || q == '+' || q == '{')
                            {
                                return true;
                            }
                        }
                        break;
                    }
                    case '|':
                    case '*':
                    case '+':
                    case '{':
                        if (!groups.empty())
                        {
                            groups.back() = true;
                        }
                        break;
                    default:
                        break;
                }
            }
            return false;
        }

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
    router_impl::set_http_handler_impl(method m, std::string_view key, coro_http_handler_type&& handler)
    {
        std::unique_lock lock(mutex_);
        auto segments = detail::split_segments(key);
        auto node = insert(root_.get(), segments, 0);
        node->handlers[m] = wrap_global(std::move(handler));
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

            if (key.size() > detail::regex_pattern_max)
            {
                throw std::invalid_argument("httplib: route regex pattern is too long (limit "
                                            + std::to_string(detail::regex_pattern_max) + "): " + std::string(key));
            }
            if (detail::has_redos_shape(key))
            {
                throw std::invalid_argument("httplib: route regex pattern rejected, it may backtrack catastrophically "
                                            "(nested quantifier or quantified alternation): "
                                            + std::string(key));
            }

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

        get_impl(req).set_path_param(util::string_map<std::string>(match.params));

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

        util::string_map<std::string> params;
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
                             util::string_map<std::string>& params,
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
        if (seg.size() <= detail::regex_subject_max)
        {
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
        if (get_impl(resp).stream_header_sent())
        {
            co_return;
        }

        if (not_found_handler_ && get_impl(resp).base().result() == http::status::not_found)
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
        std::unique_lock lock(mutex_);
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
    router_impl::set_lazy_http_handler_impl(method m, std::string_view key, coro_http_handler_type&& handler)
    {
        std::unique_lock lock(mutex_);
        auto segments = detail::split_segments(key);
        auto node = insert(root_.get(), segments, 0);
        node->lazy_handlers[m] = wrap_global(std::move(handler));
    }

    void
    router_impl::collect_allows(std::set<std::string>& allows, Node const* node)
    {
        // 键是公共的 httplib::method，要拿线上写法得先转回 beast 的 verb。
        for (auto const& v : node->handlers)
        {
            allows.insert(to_string(enum_conv::to_verb(v.first)));
        }
        for (auto const& v : node->lazy_handlers)
        {
            allows.insert(to_string(enum_conv::to_verb(v.first)));
        }
        if (node->ws_handler)
        {
            allows.insert(to_string(http::verb::get));
        }
    }

} // namespace httplib::server
