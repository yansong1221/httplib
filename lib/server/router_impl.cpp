#include "router_impl.h"
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
                                        std::string_view path,
                                        coro_http_handler_type&& handler)
{
    // std::unique_lock lock(mutex_);
    auto segments          = detail::split_segments(path);
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
            node->type                = Node::node_type::wildcard_node;
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
        node->type       = Node::node_type::param_node;
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
        node->type       = Node::node_type::regex_node;
        node->param_name = inside.substr(0, pos);
        node->regex      = std::regex(key.begin(), key.end());

        parent->regex_children.push_back(std::move(node));
        return insert(parent->regex_children.back().get(), segments, index + 1);
    }
    auto [iter, inserted] =
        parent->static_children.try_emplace(std::string(seg), std::make_unique<Node>());
    if (inserted) {
        iter->second->key  = seg;
        iter->second->type = Node::node_type::static_node;
    }
    return insert(iter->second.get(), segments, index + 1);
}


// ---------------- 匹配路由 ----------------
net::awaitable<void> router_impl::proc_routing(request& req, response& resp) const
{
    // std::shared_lock lock(mutex_);

    auto segments = detail::split_segments(req.path());

    std::unordered_map<std::string, std::string> params;
    std::set<std::string> allows;

    auto node = match_nodes(root_.get(), segments, 0, params, [&](const Node* node) {
        for (const auto& v : node->handlers)
            allows.insert(to_string(v.first));

        return node->handlers.find(req.method()) != node->handlers.end();
    });

    if (node) {
        req.set_path_param(std::move(params));
        auto iter = node->handlers.find(req.method());
        if (iter != node->handlers.end()) {
            co_await iter->second(req, resp);
            co_return;
        }
    }
    if (!allows.empty()) {
        resp.set(http::field::allow, boost::join(allows, ","));
        resp.set_error_content(httplib::http::status::method_not_allowed);
        co_return;
    }
    resp.set_error_content(httplib::http::status::not_found);
}

void router_impl::set_not_found_handler_impl(coro_http_handler_type&& handler)
{
    not_found_handler_ = std::move(handler);
}

void router_impl::set_ws_handler_impl(std::string_view path,
                                      websocket_conn::coro_open_handler_type&& open_handler,
                                      websocket_conn::coro_message_handler_type&& message_handler,
                                      websocket_conn::coro_close_handler_type&& close_handler)
{
    // std::unique_lock lock(mutex_);
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
    // std::shared_lock lock(mutex_);
    auto segments = detail::split_segments(req.path());

    std::unordered_map<std::string, std::string> params;
    auto node = match_nodes(root_.get(), segments, 0, params, [&](const Node* node) {
        return node->ws_handler.has_value();
    });

    if (!node)
        return std::nullopt;

    return node->ws_handler;
}
net::awaitable<bool> router_impl::pre_routing(request& req, response& resp) const
{
    switch (req.method()) {
        case http::verb::get:
        case http::verb::head:
        case http::verb::trace:
        case http::verb::connect:
        case http::verb::options: co_return true; break;
        default: {
            auto segments = detail::split_segments(req.path());

            std::unordered_map<std::string, std::string> params;
            std::set<std::string> allows;

            auto node = match_nodes(root_.get(), segments, 0, params, [&](const Node* node) {
                for (const auto& v : node->handlers)
                    allows.insert(to_string(v.first));

                return node->handlers.find(req.method()) != node->handlers.end();
            });

            if (node) {
                auto iter = node->handlers.find(req.method());
                if (iter != node->handlers.end()) {
                    co_return true;
                }
            }
            if (!allows.empty()) {
                resp.keep_alive(false);
                resp.set(http::field::allow, boost::join(allows, ","));
                resp.set_error_content(httplib::http::status::method_not_allowed);
                co_return false;
            }

        } break;
    }
    resp.keep_alive(false);
    resp.set_error_content(httplib::http::status::not_found);
    co_return false;
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
    if (not_found_handler_ && resp.result() == http::status::not_found)
        co_await not_found_handler_(req, resp);

    if (post_handler_)
        co_await post_handler_(req, resp);

    co_return;
}

void router_impl::set_http_post_handler_impl(coro_http_handler_type&& handler)
{
    post_handler_ = std::move(handler);
}


} // namespace httplib::server