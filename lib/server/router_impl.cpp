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

    //if (req.method() == http::verb::get || req.method() == http::verb::head) {
    //    for (const auto& entry : static_file_entry_) {
    //        if (std::invoke(*entry, req, resp))
    //            co_return;
    //    }
    //}
    std::unordered_map<std::string, std::string> path_params;

    auto segments = detail::split_segments(req.decoded_path());
    if (auto node = match_node(root_.get(), segments, 0, path_params); node) {
        auto iter = node->handlers.find(req.method());
        if (iter == node->handlers.end()) {
            resp.set_error_content(node->handlers.empty()
                                       ? httplib::http::status::not_found
                                       : httplib::http::status::method_not_allowed);
            co_return;
        }
        req.set_path_param(std::move(path_params));

        co_await iter->second(req, resp);
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
    std::unordered_map<std::string, std::string> path_params;
    auto segments = detail::split_segments(req.decoded_path());
    if (auto node = match_node(root_.get(), segments, 0, path_params); node) {
        req.set_path_param(std::move(path_params));
        return node->ws_handler;
    }

    return std::nullopt;
}
bool router_impl::pre_routing(request& req, response& resp) const
{
    switch (req.method()) {
        case http::verb::get:
        case http::verb::head:
        case http::verb::trace:
        case http::verb::connect: return true; break;
        default: {
            std::unordered_map<std::string, std::string> path_params;

            auto segments = detail::split_segments(req.decoded_path());
            if (auto node = match_node(root_.get(), segments, 0, path_params); node) {
                auto iter = node->handlers.find(req.method());
                if (iter == node->handlers.end()) {
                    resp.keep_alive(false);
                    resp.set_error_content(node->handlers.empty()
                                               ? httplib::http::status::not_found
                                               : httplib::http::status::method_not_allowed);
                    return false;
                }
                return true;
            }

        } break;
    }
    resp.keep_alive(false);
    resp.set_error_content(httplib::http::status::not_found);
    return false;
}

// ---------------- match_node ----------------
const router_impl::Node*
router_impl::match_node(const Node* node,
                        const std::vector<std::string_view>& segments,
                        size_t index,
                        std::unordered_map<std::string, std::string>& path_params) const
{
    if (!node)
        return nullptr;

    if (index == segments.size())
        return node;

    const auto& seg = segments[index];

    // 1️⃣ 静态匹配
    for (auto& child : node->children) {
        if (!child->is_param && !child->is_regex && !child->is_wildcard) {
            if (child->key == seg) {
                if (auto node = match_node(child.get(), segments, index + 1, path_params); node)
                    return node;
            }
        }
    }

    // 2️⃣ 正则匹配
    for (auto& child : node->children) {
        if (child->is_regex) {
            if (std::regex_match(seg.begin(), seg.end(), child->regex)) {
                path_params[child->param_name] = seg;
                if (auto node = match_node(child.get(), segments, index + 1, path_params); node)
                    return node;
                path_params.erase(child->param_name);
            }
        }
    }

    // 3️⃣ 动态参数匹配
    for (auto& child : node->children) {
        if (child->is_param) {
            path_params[child->param_name] = seg;
            if (auto node = match_node(child.get(), segments, index + 1, path_params); node)
                return node;
            path_params.erase(child->param_name);
        }
    }

    // 4️⃣ wildcard 匹配
    for (auto& child : node->children) {
        if (child->is_wildcard) {
            // 贪心 + 回溯匹配
            for (size_t len = 1; index + len <= segments.size(); ++len) {
                std::string captured;
                for (size_t i = 0; i < len; ++i) {
                    if (!captured.empty())
                        captured += "/";
                    captured += segments[index + i];
                }
                path_params["*"] = captured;
                if (auto node = match_node(child.get(), segments, index + len, path_params); node)
                    return node;
                path_params.erase("*");
            }
            /*std::string rest;
            for (size_t i = index; i < segments.size(); ++i) {
                if (!rest.empty())
                    rest += "/";
                rest += segments[i];
            }
            path_params["*"] = rest;
            return child.get();*/
        }
    }
    return nullptr;
}

} // namespace httplib::server