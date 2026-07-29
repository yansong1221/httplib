#include "router_impl.h"
#include "request_impl.hpp"
#include "response_impl.hpp"
#include <boost/algorithm/string/join.hpp>
#include <iostream>
#include <set>

namespace httplib::server {

namespace detail {

static auto split_segments(std::string_view path)
{
    auto segments = util::split(path, "/");

    if (path.ends_with("/"))
        segments.push_back(std::string_view());

    return segments;
}

} // namespace detail

router_impl::router_impl()
    : root_(std::make_unique<Node>())
{
}

void router_impl::set_http_handler_impl(http::verb method,
                                        std::string_view key,
                                        coro_http_handler_type&& handler)
{
    std::unique_lock lock(mutex_);
    auto segments          = detail::split_segments(key);
    auto node              = insert(root_.get(), segments, 0);
    node->handlers[method] = std::move(handler);
}


router_impl::Node* router_impl::insert(Node* parent,
                                       const std::vector<std::string_view>& segments,
                                       size_t index)
{
    if (index >= segments.size())
        return parent;

    const auto& seg = segments.at(index);

    if (segments.size() - 1 == index && seg == "*") {
        if (!parent->wildcard_children) {
            auto node                 = std::make_unique<Node>();
            node->key                 = seg;
            parent->wildcard_children = std::move(node);
        }
        return insert(parent->wildcard_children.get(), segments, index + 1);
    }

    if (!seg.empty() && seg.starts_with(":")) {
        auto iter = std::ranges::find_if(parent->param_children,
                                         [&](const auto& node) { return node->key == seg; });

        if (iter != parent->param_children.end())
            return insert(iter->get(), segments, index + 1);

        auto node        = std::make_unique<Node>();
        node->key        = seg;
        node->param_name = seg.substr(1);

        parent->param_children.push_back(std::move(node));
        return insert(parent->param_children.back().get(), segments, index + 1);
    }

    if (!seg.empty() && seg.front() == '{' && seg.back() == '}') {
        auto iter = std::ranges::find_if(parent->regex_children,
                                         [&](const auto& node) { return node->key == seg; });

        if (iter != parent->regex_children.end())
            return insert(iter->get(), segments, index + 1);

        std::string_view inside = seg.substr(1, seg.size() - 2);
        size_t pos              = inside.find(':');
        auto key                = inside.substr(pos + 1);

        auto node        = std::make_unique<Node>();
        node->key        = seg;
        node->param_name = inside.substr(0, pos);
        node->regex      = std::regex(key.begin(), key.end());

        parent->regex_children.push_back(std::move(node));
        return insert(parent->regex_children.back().get(), segments, index + 1);
    }
    auto [iter, inserted] = parent->static_children.try_emplace(std::string(seg), nullptr);
    if (inserted) {
        iter->second       = std::make_unique<Node>();
        iter->second->key  = seg;
    }
    return insert(iter->second.get(), segments, index + 1);
}


// ---------------- 匹配路由 ----------------
net::awaitable<void>
router_impl::process_routing(const route_match& match, request& req, response& resp) const
{
    if (!match.node) {
        write_error(resp, match.allows);
        co_return;
    }

    req.set_path_param(std::unordered_map<std::string, std::string>(match.params));

    if (match.body == body_kind::chunked) {
        auto iter = match.node->chunked_handlers.find(req.method());
        if (iter != match.node->chunked_handlers.end())
            co_await iter->second(req, resp);
    }
    else if (match.body == body_kind::buffer_body) {
        auto iter = match.node->buffer_body_handlers.find(req.method());
        if (iter != match.node->buffer_body_handlers.end())
            co_await iter->second(req, resp);
    }
    else {
        auto iter = match.node->handlers.find(req.method());
        if (iter != match.node->handlers.end())
            co_await iter->second(req, resp);
    }
}

void router_impl::set_not_found_handler_impl(coro_http_handler_type&& handler)
{ not_found_handler_ = std::move(handler); }

void router_impl::set_ws_handler_impl(std::string_view path,
                                      websocket_conn::coro_open_handler_type&& open_handler,
                                      websocket_conn::coro_message_handler_type&& message_handler,
                                      websocket_conn::coro_close_handler_type&& close_handler)
{
    std::unique_lock lock(mutex_);
    auto segments = detail::split_segments(path);

    auto node = insert(root_.get(), segments, 0);

    ws_handler_entry entry;
    entry.open_handler    = std::move(open_handler);
    entry.message_handler = std::move(message_handler);
    entry.close_handler   = std::move(close_handler);
    node->ws_handler      = std::move(entry);
}

std::optional<router_impl::ws_handler_entry> router_impl::query_ws_handler(request& req) const
{
    std::shared_lock lock(mutex_);
    auto segments = detail::split_segments(req.path());

    std::unordered_map<std::string, std::string> params;
    auto node = match_nodes(root_.get(), segments, 0, params, [&](const Node* node) {
        return node->ws_handler.has_value();
    });

    if (!node)
        return std::nullopt;

    return node->ws_handler;
}
net::awaitable<router_impl::route_match> router_impl::pre_routing(request& req) const
{
    route_match result;

    std::shared_lock lock(mutex_);
    auto segments = detail::split_segments(req.path());
    bool is_chunked_te = req.get_impl()->chunked();

    auto node = match_nodes(root_.get(), segments, 0, result.params, [&](const Node* node) {
        collect_allows(result.allows, node, is_chunked_te);
        if (node->handlers.find(req.method()) != node->handlers.end())
            return true;
        if (is_chunked_te &&
            node->chunked_handlers.find(req.method()) != node->chunked_handlers.end())
            return true;
        if (node->buffer_body_handlers.find(req.method()) != node->buffer_body_handlers.end())
            return true;
        return false;
    });

    if (!node) {
        co_return result;
    }

    result.node = node;

    switch (req.method()) {
    case http::verb::get:
    case http::verb::head:
    case http::verb::trace:
    case http::verb::connect:
    case http::verb::options:
        result.body = body_kind::none;
        co_return result;
    default:
        break;
    }

    if (node->handlers.find(req.method()) != node->handlers.end()) {
        result.body = body_kind::none;
        co_return result;
    }
    if (is_chunked_te &&
        node->chunked_handlers.find(req.method()) != node->chunked_handlers.end()) {
        result.body = body_kind::chunked;
        co_return result;
    }
    if (node->buffer_body_handlers.find(req.method()) != node->buffer_body_handlers.end()) {
        result.body = body_kind::buffer_body;
        co_return result;
    }
    co_return result;
}

const router_impl::Node*
router_impl::match_nodes(const Node* parent,
                         const std::vector<std::string_view>& segments,
                         size_t index,
                         std::unordered_map<std::string, std::string>& params,
                         const MatchHandlerType& handler) const
{
    if (!parent)
        return nullptr;

    if (index == segments.size()) {
        if (!handler(parent))
            return nullptr;
        return parent;
    }

    const auto& seg = segments[index];

    // 1) static
    {
        auto iter = parent->static_children.find(std::string(seg));
        if (iter != parent->static_children.end()) {
            if (auto node = match_nodes(iter->second.get(), segments, index + 1, params, handler);
                node)
                return node;
        }
    }

    // 2) regex
    for (auto& child : parent->regex_children) {
        if (std::regex_match(seg.data(), seg.data() + seg.length(), child->regex)) {
            params[child->param_name] = std::string(seg);
            if (auto node = match_nodes(child.get(), segments, index + 1, params, handler); node)
                return node;

            params.erase(child->param_name);
        }
    }

    // 3) param
    for (auto& child : parent->param_children) {
        params[child->param_name] = std::string(seg);
        if (auto node = match_nodes(child.get(), segments, index + 1, params, handler); node)
            return node;
        params.erase(child->param_name);
    }

    // 4) wildcard
    if (parent->wildcard_children) {
        std::string rest;
        for (size_t i = index; i < segments.size(); ++i) {
            if (!rest.empty())
                rest += "/";
            rest += segments[i];
        }
        params["*"] = std::move(rest);
        if (auto node = match_nodes(
                parent->wildcard_children.get(), segments, segments.size(), params, handler);
            node)
            return node;
        params.erase("*");
    }
    return nullptr;
}

net::awaitable<void> router_impl::post_routing(request& req, response& resp) const
{
    if (not_found_handler_ && resp.get_impl()->result() == http::status::not_found)
        co_await not_found_handler_(req, resp);

    if (post_routing_handler_)
        co_await post_routing_handler_(req, resp);

    co_return;
}

void router_impl::set_post_routing_handler_impl(coro_http_handler_type&& handler)
{ post_routing_handler_ = std::move(handler); }

void router_impl::set_chunked_http_handler_impl(http::verb method,
                                                std::string_view key,
                                                coro_http_handler_type&& handler)
{
    std::unique_lock lock(mutex_);
    auto segments                  = detail::split_segments(key);
    auto node                      = insert(root_.get(), segments, 0);
    node->chunked_handlers[method] = std::move(handler);
}

void router_impl::set_buffer_body_http_handler_impl(http::verb method,
                                               std::string_view key,
                                               coro_http_handler_type&& handler)
{
    std::unique_lock lock(mutex_);
    auto segments                      = detail::split_segments(key);
    auto node                          = insert(root_.get(), segments, 0);
    node->buffer_body_handlers[method] = std::move(handler);
}

void router_impl::collect_allows(std::set<std::string>& allows, const Node* node, bool include_chunked)
{
    for (const auto& v : node->handlers)
        allows.insert(to_string(v.first));
    for (const auto& v : node->buffer_body_handlers)
        allows.insert(to_string(v.first));
    if (include_chunked) {
        for (const auto& v : node->chunked_handlers)
            allows.insert(to_string(v.first));
    }
}

void router_impl::write_error(response& resp, const std::set<std::string>& allows)
{
    if (!allows.empty()) {
        resp.set(http::field::allow, boost::join(allows, ","));
        resp.set_error_content(httplib::http::status::method_not_allowed);
        return;
    }
    resp.set_error_content(httplib::http::status::not_found);
}

} // namespace httplib::server