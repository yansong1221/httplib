#include "router_impl.h"
#include <iostream>
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
{
    root_      = std::make_unique<Node>();
    root_->key = "";
}

std::unique_ptr<router_impl::Node> router_impl::make_special_node(std::string_view seg)
{
    auto node = std::make_unique<Node>();
    node->key = seg;

    if (seg == "*") {
        node->is_wildcard = true;
        return node;
    }

    if (!seg.empty() && seg[0] == ':') {
        node->is_param   = true;
        node->param_name = seg.substr(1);
        return node;
    }

    if (!seg.empty() && seg.front() == '{' && seg.back() == '}') {
        node->is_regex          = true;
        std::string_view inside = seg.substr(1, seg.size() - 2);
        size_t pos              = inside.find(':');
        node->param_name        = inside.substr(0, pos);

        auto key = inside.substr(pos + 1);
        node->regex.assign(key.begin(), key.end());
        return node;
    }

    return node;
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


router_impl::Node* router_impl::insert(Node* node,
                                       const std::vector<std::string_view>& segments,
                                       size_t index)
{
    if (index >= segments.size())
        return node;

    const auto& seg = segments[index];
    Node* found     = nullptr;

    for (auto& child : node->children) {
        if (child->key == seg)
            found = child.get();
    }

    if (!found) {
        auto new_node = make_special_node(seg);
        found         = new_node.get();
        node->children.push_back(std::move(new_node));
    }
    return insert(found, segments, index + 1);
}


// ---------------- 匹配路由 ----------------
net::awaitable<void> router_impl::proc_routing(request& req, response& resp) const
{
    // std::shared_lock lock(mutex_);

    // if (req.method() == http::verb::get || req.method() == http::verb::head) {
    //     for (const auto& entry : static_file_entry_) {
    //         if (std::invoke(*entry, req, resp))
    //             co_return;
    //     }
    // }

    auto segments = detail::split_segments(req.decoded_path());

    std::vector<MatchResult> results;
    match_nodes(root_.get(), segments, 0, results);
    if (!results.empty()) {
        for (auto& item : results) {
            auto iter = item.node->handlers.find(req.method());
            if (iter == item.node->handlers.end())
                continue;

            req.set_path_param(std::move(item.params));
            co_await iter->second(req, resp);
            co_return;
        }
        resp.set_error_content(httplib::http::status::method_not_allowed);
        co_return;
    }

    if (default_handler_) {
        co_await default_handler_(req, resp);
        co_return;
    }
    resp.set_error_content(httplib::http::status::not_found);
}

void router_impl::set_default_handler_impl(coro_http_handler_type&& handler)
{
    default_handler_ = std::move(handler);
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
    auto segments = detail::split_segments(req.decoded_path());

    std::vector<MatchResult> results;
    match_nodes(root_.get(), segments, 0, results);

    for (auto& item : results) {
        if (item.node->ws_handler) {
            req.set_path_param(std::move(item.params));
            return item.node->ws_handler;
        }
    }
    return std::nullopt;
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
            auto segments = detail::split_segments(req.decoded_path());

            std::vector<MatchResult> results;
            match_nodes(root_.get(), segments, 0, results);
            if (!results.empty()) {
                for (auto& item : results) {
                    auto iter = item.node->handlers.find(req.method());
                    if (iter == item.node->handlers.end())
                        continue;

                    co_return true;
                }
                resp.keep_alive(false);
                resp.set_error_content(httplib::http::status::method_not_allowed);
                co_return false;
            }

        } break;
    }
    resp.keep_alive(false);
    resp.set_error_content(httplib::http::status::not_found);
    co_return false;
}

void router_impl::match_nodes(const Node* node,
                              const std::vector<std::string_view>& segments,
                              size_t index,
                              std::vector<MatchResult>& results,
                              std::unordered_map<std::string, std::string> params) const
{
    if (!node)
        return;

    if (index == segments.size()) {
        results.push_back({node, std::move(params)});
        return;
    }

    const auto& seg = segments[index];

    // 1) static
    for (auto& child : node->children) {
        if (!child->is_param && !child->is_regex && !child->is_wildcard) {
            if (child->key == seg) {
                match_nodes(child.get(), segments, index + 1, results, params);
            }
        }
    }

    // 2) regex
    for (auto& child : node->children) {
        if (child->is_regex) {
            if (std::regex_match(seg.data(), seg.data() + seg.length(), child->regex)) {
                auto p2               = params;
                p2[child->param_name] = std::string(seg);
                match_nodes(child.get(), segments, index + 1, results, std::move(p2));
            }
        }
    }

    // 3) param
    for (auto& child : node->children) {
        if (child->is_param) {
            auto p2               = params;
            p2[child->param_name] = std::string(seg);
            match_nodes(child.get(), segments, index + 1, results, std::move(p2));
        }
    }

    // 4) wildcard
    for (auto& child : node->children) {
        if (!child->is_wildcard)
            continue;

        // 终节点：吃全部剩余
        if (child->children.empty()) {
            auto p2 = params;
            std::string rest;
            for (size_t i = index; i < segments.size(); ++i) {
                if (!rest.empty())
                    rest += "/";
                rest += segments[i];
            }
            p2["*"] = std::move(rest);
            results.push_back({child.get(), std::move(p2)});
            continue;
        }

        // 吃一个
        {
            auto p2 = params;
            p2["*"] = std::string(seg);
            match_nodes(child.get(), segments, index + 1, results, std::move(p2));
        }

        // 吃全部剩余
        {
            auto p2 = params;
            std::string rest;
            for (size_t i = index; i < segments.size(); ++i) {
                if (!rest.empty())
                    rest += "/";
                rest += segments[i];
            }
            p2["*"] = std::move(rest);
            match_nodes(child.get(), segments, segments.size(), results, std::move(p2));
        }
    }
}

net::awaitable<void> router_impl::post_routing(request& req, response& resp) const
{
    if (post_handler_)
        co_await post_handler_(req, resp);

    co_return;
}

void router_impl::set_http_post_handler_impl(coro_http_handler_type&& handler)
{
    post_handler_ = std::move(handler);
}

} // namespace httplib::server