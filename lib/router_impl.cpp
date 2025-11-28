#include "router_impl.h"
#include <iostream>
namespace httplib {

namespace detail {
inline static bool is_valid_path(std::string_view path)
{
    size_t level = 0;
    size_t i     = 0;

    // Skip slash
    while (i < path.size() && path[i] == '/') {
        i++;
    }

    while (i < path.size()) {
        // Read component
        auto beg = i;
        while (i < path.size() && path[i] != '/') {
            if (path[i] == '\0') {
                return false;
            }
            else if (path[i] == '\\') {
                return false;
            }
            i++;
        }

        auto len = i - beg;
        assert(len > 0);

        if (!path.compare(beg, len, ".")) {
            ;
        }
        else if (!path.compare(beg, len, "..")) {
            if (level == 0) {
                return false;
            }
            level--;
        }
        else {
            level++;
        }

        // Skip slash
        while (i < path.size() && path[i] == '/') {
            i++;
        }
    }

    return true;
}

} // namespace detail

router_impl::router_impl()
{
    root_      = std::make_unique<Node>();
    root_->key = "";
}

// ---------------- 特殊节点 ----------------
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

    if (seg.front() == '{' && seg.back() == '}') {
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
    auto segments          = util::split(path, "/");
    auto node              = insert(root_.get(), segments, 0);
    node->handlers[method] = std::move(handler);
}


httplib::router_impl::Node* router_impl::insert(Node* node,
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
net::awaitable<void> router_impl::proc_routing(request_impl& req, response_impl& resp) const
{
    // std::shared_lock lock(mutex_);

    if (req.header().method() == http::verb::get || req.header().method() == http::verb::head) {
        if (co_await handle_file_request(req, resp))
            co_return;
    }

    auto segments = util::split(req.decoded_path(), "/");
    if (auto node = match_node(root_.get(), segments, 0, req.path_params); node) {
        auto iter = node->handlers.find(req.header().method());
        if (iter == node->handlers.end()) {
            resp.set_error_content(node->handlers.empty()
                                       ? httplib::http::status::not_found
                                       : httplib::http::status::method_not_allowed);
            co_return;
        }
        co_await iter->second(req, resp);
        co_return;
    }

    if (default_handler_) {
        co_await default_handler_(req, resp);
        co_return;
    }

    resp.set_error_content(httplib::http::status::not_found);
}

bool router_impl::set_mount_point(const std::string& mount_point,
                                  const std::filesystem::path& dir,
                                  const http::fields& headers /*= {}*/)
{
    std::error_code ec;
    if (fs::is_directory(dir, ec)) {
        std::string mnt = !mount_point.empty() ? mount_point : "/";
        if (!mnt.empty() && mnt[0] == '/') {
            static_file_entry_.push_back({mnt, dir, headers});
            std::sort(static_file_entry_.begin(),
                      static_file_entry_.end(),
                      [](const auto& left, const auto& right) {
                          return left.mount_point.size() > right.mount_point.size();
                      });
            return true;
        }
    }
    return false;
}

bool router_impl::remove_mount_point(const std::string& mount_point)
{
    for (auto it = static_file_entry_.begin(); it != static_file_entry_.end(); ++it) {
        if (it->mount_point == mount_point) {
            static_file_entry_.erase(it);
            return true;
        }
    }
    return false;
}

void router_impl::set_default_handler_impl(coro_http_handler_type&& handler)
{
    default_handler_ = std::move(handler);
}

void router_impl::set_file_request_handler_impl(coro_http_handler_type&& handler)
{
    file_request_handler_ = std::move(handler);
}

void router_impl::set_ws_handler_impl(std::string_view path,
                                      websocket_conn::coro_open_handler_type&& open_handler,
                                      websocket_conn::coro_message_handler_type&& message_handler,
                                      websocket_conn::coro_close_handler_type&& close_handler)
{
    // std::unique_lock lock(mutex_);
    auto segments = util::split(path, "/");

    auto node = insert(root_.get(), segments, 0);

    ws_handler_entry entry;
    entry.open_handler    = std::move(open_handler);
    entry.message_handler = std::move(message_handler);
    entry.close_handler   = std::move(close_handler);
    node->ws_handler      = std::move(entry);
}

std::optional<router_impl::ws_handler_entry> router_impl::find_ws_handler(request& req) const
{
    // std::shared_lock lock(mutex_);
    auto segments = util::split(req.decoded_path(), "/");
    if (auto node = match_node(root_.get(), segments, 0, req.path_params); node)
        return node->ws_handler;
    return std::nullopt;
}

bool router_impl::has_handler(http::verb method, std::string_view target) const
{
    return true;
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

httplib::net::awaitable<bool> router_impl::handle_file_request(request& req, response& res) const
{
    beast::error_code ec;

    for (const auto& entry : static_file_entry_) {
        std::string_view target(req.decoded_path());
        // Prefix match
        if (!target.starts_with(entry.mount_point))
            continue;
        target.remove_prefix(entry.mount_point.size());
        if (target.starts_with("/"))
            target.remove_prefix(1);

        if (!detail::is_valid_path(target))
            continue;

        auto path = entry.base_dir /
                    fs::path(std::u8string_view((const char8_t*)target.data(), target.size()));
        if (!fs::exists(path, ec))
            continue;

        if (target.empty() && !req.decoded_path().ends_with("/")) {
            res.set_redirect(std::string(req.decoded_path()) + "/");
            co_return true;
        }

        if (!path.has_filename()) {
            for (const auto& doc_name : default_doc_name_) {
                auto doc_path = path / doc_name;

                boost::system::error_code ec;
                if (!fs::is_regular_file(doc_path, ec))
                    continue;

                path = doc_path;
                break;
            }
        }
        if (path.has_filename()) {
            if (fs::is_regular_file(path, ec)) {
                for (const auto& kv : entry.headers) {
                    res.header().set(kv.name_string(), kv.value());
                }
                res.set_file_content(path, req.header());
                if (req.header().method() != http::verb::head && file_request_handler_) {
                    co_await file_request_handler_(req, res);
                }

                co_return true;
            }
        }
        else if (fs::is_directory(path, ec)) {
            beast::error_code ec;
            auto body = html::format_dir_to_html(req.decoded_path(), path, ec);
            if (ec)
                co_return false;
            res.set_string_content(body, "text/html; charset=utf-8");
            co_return true;
        }
    }

    co_return false;
}

} // namespace httplib