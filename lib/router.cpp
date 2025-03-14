#include "httplib/router.hpp"

#include "httplib/html.hpp"
#include "httplib/http_handler.hpp"
#include "httplib/util/type_traits.h"
#include "radix_tree.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/beast/version.hpp>
#include <functional>
#include <regex>
#include <set>
#include <spdlog/spdlog.h>
#include <tuple>
#include <unordered_map>

namespace httplib {
namespace detail {
inline static bool
is_valid_path(std::string_view path)
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
            } else if (path[i] == '\\') {
                return false;
            }
            i++;
        }

        auto len = i - beg;
        assert(len > 0);

        if (!path.compare(beg, len, ".")) {
            ;
        } else if (!path.compare(beg, len, "..")) {
            if (level == 0) { return false; }
            level--;
        } else {
            level++;
        }

        // Skip slash
        while (i < path.size() && path[i] == '/') {
            i++;
        }
    }

    return true;
}

static std::string
make_whole_str(http::verb method, std::string_view target)
{
    return fmt::format("{} {}", std::string_view(http::to_string(method)), target);
}
static std::string
make_whole_str(const request& req)
{
    return make_whole_str(req.base().method(), util::url_decode(req.target()));
}

} // namespace detail

class router::impl {
public:
    impl(std::shared_ptr<spdlog::logger> logger) : logger_(logger) { }

    net::awaitable<bool>
    handle_file_request(request& req, response& res)
    {
        beast::error_code ec;

        for (const auto& entry : static_file_entry_) {
            std::string_view target(req.path);
            // Prefix match
            if (!target.starts_with(entry.mount_point)) continue;
            target.remove_prefix(entry.mount_point.size());
            if (!detail::is_valid_path(target)) continue;

            auto path = entry.base_dir /
                        fs::path(std::u8string_view((const char8_t*)target.data(),
                                                    target.size()));
            if (!fs::exists(path, ec)) continue;

            if (target.empty() && !req.path.ends_with("/")) {
                res.set_redirect(req.path + "/");
                co_return true;
            }

            if (!path.has_filename()) {
                for (const auto& doc_name : default_doc_name_) {
                    auto doc_path = path / doc_name;

                    boost::system::error_code ec;
                    if (!fs::is_regular_file(doc_path, ec)) continue;

                    path = doc_path;
                    break;
                }
            }
            if (path.has_filename()) {
                if (fs::is_regular_file(path, ec)) {
                    for (const auto& kv : entry.headers) {
                        res.base().set(kv.name_string(), kv.value());
                    }
                    res.set_file_content(path, req);
                    if (req.method() != http::verb::head && file_request_handler_) {
                        co_await file_request_handler_(req, res);
                    }

                    co_return true;
                }
            } else if (fs::is_directory(path, ec)) {
                beast::error_code ec;
                auto body = html::format_dir_to_html(req.path, path, ec);
                if (ec) co_return false;
                res.set_string_content(body, "text/html; charset=utf-8");
                co_return true;
            }
        }

        co_return false;
    }
    net::awaitable<void>
    proc_routing(request& req, response& resp)
    {
        if (req.method() == http::verb::get || req.method() == http::verb::head) {
            if (co_await handle_file_request(req, resp)) co_return;
        }

        {
            auto iter = coro_handles_.find(req.path);
            if (iter != coro_handles_.end()) {
                const auto& map = iter->second;
                auto iter       = map.find(req.method());
                if (iter != map.end()) {
                    co_await iter->second(req, resp);
                    co_return;
                } else {
                    resp.set_error_content(http::status::method_not_allowed);
                    co_return;
                }
            }
        }
        if (default_handler_) {
            co_await default_handler_(req, resp);
            co_return;
        }
        auto key             = detail::make_whole_str(req);
        std::string url_path = detail::make_whole_str(req.method(), req.target());

        bool is_coro_exist = false;
        coro_http_handler_type coro_handler;
        std::tie(is_coro_exist, coro_handler, req.path_params) =
            coro_router_tree_->get_coro(url_path, req.method());

        if (is_coro_exist) {
            if (coro_handler) {
                co_await coro_handler(req, resp);
            } else {
                resp.set_error_content(http::status::not_found);
            }
            co_return;
        }
        bool is_matched_regex_router = false;
        // coro regex router
        for (auto& pair : coro_regex_handles_) {
            std::string coro_regex_key {key};

            if (std::regex_match(coro_regex_key, req.matches, std::get<0>(pair))) {
                auto coro_handler = std::get<1>(pair);
                if (coro_handler) {
                    co_await coro_handler(req, resp);
                    is_matched_regex_router = true;
                }
            }
        }

        // not found
        if (!is_matched_regex_router) { resp.set_error_content(http::status::not_found); }
        co_return;
    }

public:
    std::shared_ptr<spdlog::logger> logger_;

    using verb_handler_map = std::unordered_map<http::verb, coro_http_handler_type>;
    std::unordered_map<std::string, verb_handler_map> coro_handles_;

    std::shared_ptr<radix_tree> coro_router_tree_ =
        std::make_shared<radix_tree>(radix_tree());
    std::vector<std::tuple<std::regex, coro_http_handler_type>> coro_regex_handles_;

    coro_http_handler_type default_handler_;
    coro_http_handler_type file_request_handler_;

    struct mount_point_entry {
        std::string mount_point;
        fs::path base_dir;
        http::fields headers;
    };
    std::vector<mount_point_entry> static_file_entry_;

    std::vector<std::string> default_doc_name_ = {"index.html", "index.htm"};
};
router::router(std::shared_ptr<spdlog::logger> logger) : impl_(new impl(logger)) { }
router::~router()
{
    // delete impl_;
}
bool
router::has_handler(http::verb method, std::string_view target) const
{
    return true;
}
net::awaitable<void>
router::routing(request& req, response& resp)
{
    try {
        co_await impl_->proc_routing(req, resp);
    } catch (const std::exception& e) {
        impl_->logger_->warn("exception in business function, reason: {}", e.what());
        resp.set_string_content(
            std::string_view(e.what()), "text/html", http::status::internal_server_error);
    } catch (...) {
        using namespace std::string_view_literals;
        impl_->logger_->warn("unknown exception in business function");
        resp.set_string_content(
            "unknown exception"sv, "text/html", http::status::internal_server_error);
    }
}
bool
router::set_mount_point(const std::string& mount_point,
                        const fs::path& dir,
                        const http::fields& headers /*= {}*/)
{
    if (fs::is_directory(dir)) {
        std::string mnt = !mount_point.empty() ? mount_point : "/";
        if (!mnt.empty() && mnt[0] == '/') {
            impl_->static_file_entry_.push_back({mnt, dir, headers});
            std::sort(impl_->static_file_entry_.begin(),
                      impl_->static_file_entry_.end(),
                      [](const auto& left, const auto& right) {
                          return left.mount_point.size() > right.mount_point.size();
                      });
            return true;
        }
    }
    impl_->logger_->warn("set_mount_point path: {} is not directory", dir.string());
    return false;
}
bool
router::remove_mount_point(const std::string& mount_point)
{
    for (auto it = impl_->static_file_entry_.begin();
         it != impl_->static_file_entry_.end();
         ++it) {
        if (it->mount_point == mount_point) {
            impl_->static_file_entry_.erase(it);
            return true;
        }
    }
    return false;
}

void
router::set_http_handler_impl(http::verb method,
                              std::string_view key,
                              coro_http_handler_type&& handler)
{
    auto whole_str = detail::make_whole_str(method, key);

    if (whole_str.find(":") != std::string::npos) {
        impl_->coro_router_tree_->coro_insert(whole_str, std::move(handler), method);
        return;
    }

    if (whole_str.find("{") != std::string::npos ||
        whole_str.find(")") != std::string::npos) {
        std::string pattern = whole_str;

        if (pattern.find("{}") != std::string::npos) {
            boost::replace_all(pattern, "{}", "([^/]+)");
        }

        impl_->coro_regex_handles_.emplace_back(std::regex(pattern), std::move(handler));
        return;
    }
    auto& map = impl_->coro_handles_[std::string(key)];
    if (map.count(method)) {
        impl_->logger_->warn("router method: {} key: {} "
                             "has already registered.",
                             http::to_string(method),
                             key);
        return;
    }
    map[method] = std::move(handler);
}

void
router::set_default_handler_impl(coro_http_handler_type&& handler)
{
    impl_->default_handler_ = std::move(handler);
}

void
router::set_file_request_handler_impl(coro_http_handler_type&& handler)
{
    impl_->file_request_handler_ = std::move(handler);
}

} // namespace httplib