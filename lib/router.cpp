#include "httplib/router.hpp"

#include "html.hpp"
#include "httplib/http_handler.hpp"
#include "httplib/request.hpp"
#include "httplib/response.hpp"
#include "httplib/util/type_traits.h"
#include "httplib/variant_message.hpp"
#include "radix_tree.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/beast/version.hpp>
#include <functional>
#include <regex>
#include <set>
#include <spdlog/spdlog.h>
#include <tuple>
#include <unordered_map>

namespace httplib
{
namespace detail
{
inline static bool is_valid_path(std::string_view path)
{
    size_t level = 0;
    size_t i = 0;

    // Skip slash
    while (i < path.size() && path[i] == '/')
    {
        i++;
    }

    while (i < path.size())
    {
        // Read component
        auto beg = i;
        while (i < path.size() && path[i] != '/')
        {
            if (path[i] == '\0')
            {
                return false;
            }
            else if (path[i] == '\\')
            {
                return false;
            }
            i++;
        }

        auto len = i - beg;
        assert(len > 0);

        if (!path.compare(beg, len, "."))
        {
            ;
        }
        else if (!path.compare(beg, len, ".."))
        {
            if (level == 0)
            {
                return false;
            }
            level--;
        }
        else
        {
            level++;
        }

        // Skip slash
        while (i < path.size() && path[i] == '/')
        {
            i++;
        }
    }

    return true;
}

static std::string make_whole_str(http::verb method, std::string_view target)
{
    return fmt::format("{} {}", std::string_view(http::to_string(method)), target);
}
static std::string make_whole_str(const request& req)
{
    return make_whole_str(req.base().method(), util::url_decode(req.target()));
}

} // namespace detail

class router::impl
{
public:
    impl(std::shared_ptr<spdlog::logger> logger) : logger_(logger) { }

    net::awaitable<bool> handle_file_request(request& req, response& res)
    {
        std::string decoded_target = util::url_decode(req.target());
        std::string_view target(decoded_target);
        beast::error_code ec;

        for (const auto& entry : static_file_entry_)
        {
            // Prefix match
            if (!target.starts_with(entry.mount_point)) continue;
            target.remove_prefix(entry.mount_point.size());
            if (!detail::is_valid_path(target)) continue;

            auto path = entry.base_dir / std::filesystem::u8path(target);
            if (!std::filesystem::exists(path, ec)) continue;

            if (std::filesystem::is_directory(path, ec))
            {
                if (auto html_path = path / "index.html"; std::filesystem::is_regular_file(html_path, ec))
                {
                    path = html_path;
                }
                else if (auto htm_path = path / "index.htm"; std::filesystem::is_regular_file(htm_path, ec))
                {
                    path = htm_path;
                }
            }

            if (std::filesystem::is_directory(path, ec))
            {
                beast::error_code ec;
                auto body = html::format_dir_to_html(req.target(), path, ec);
                if (ec) co_return false;
                res.set_string_content(body, "text/html");
                co_return true;
            }

            if (std::filesystem::is_regular_file(path, ec))
            {
                for (const auto& kv : entry.headers)
                {
                    res.base().set(kv.name(), kv.value());
                }
                beast::error_code ec;
                res.set_file_content(path, ec);
                if (ec)
                {
                    logger_->warn("set_file_content： {}", ec.message());
                    co_return false;
                }
                if (req.method() != http::verb::head && file_request_handler_)
                {
                    co_await file_request_handler_(req, res);
                }

                co_return true;
            }
        }

        co_return false;
    }
    net::awaitable<void> proc_routing_befor(request& req, response& resp) { co_return; }
    net::awaitable<void> proc_routing_after(request& req, response& resp) { co_return; }
    net::awaitable<void> proc_routing(request& req, response& resp)
    {
        if (req.method() == http::verb::get || req.method() == http::verb::head)
        {
            if (co_await handle_file_request(req, resp)) co_return;
        }

        {
            auto iter = coro_handles_.find(util::url_decode(req.target()));
            if (iter != coro_handles_.end())
            {
                const auto& map = iter->second;
                auto iter = map.find(req.method());
                if (iter != map.end())
                {
                    co_await iter->second(req, resp);
                    co_return;
                }
                else
                {
                    resp.base().result(http::status::method_not_allowed);
                    co_return;
                }
            }
        }
        if (default_handler_)
        {
            co_await default_handler_(req, resp);
            co_return;
        }
        auto key = detail::make_whole_str(req);
        std::string url_path = detail::make_whole_str(req.method(), req.target());

        bool is_coro_exist = false;
        coro_http_handler_type coro_handler;
        std::tie(is_coro_exist, coro_handler, req.params) = coro_router_tree_->get_coro(url_path, req.method());

        if (is_coro_exist)
        {
            if (coro_handler)
            {
                co_await coro_handler(req, resp);
            }
            else
            {
                resp.base().result(http::status::not_found);
            }
            co_return;
        }
        bool is_matched_regex_router = false;
        // coro regex router
        for (auto& pair : coro_regex_handles_)
        {
            std::string coro_regex_key {key};

            if (std::regex_match(coro_regex_key, req.matches, std::get<0>(pair)))
            {
                auto coro_handler = std::get<1>(pair);
                if (coro_handler)
                {
                    co_await coro_handler(req, resp);
                    is_matched_regex_router = true;
                }
            }
        }

        // not found
        if (!is_matched_regex_router)
        {
            resp.base().result(http::status::not_found);
        }
        co_return;
    }

public:
    std::shared_ptr<spdlog::logger> logger_;

    using verb_handler_map = std::unordered_map<http::verb, coro_http_handler_type>;
    std::unordered_map<std::string, verb_handler_map> coro_handles_;

    std::shared_ptr<radix_tree> coro_router_tree_ = std::make_shared<radix_tree>(radix_tree());
    std::vector<std::tuple<std::regex, coro_http_handler_type>> coro_regex_handles_;

    coro_http_handler_type default_handler_;
    coro_http_handler_type file_request_handler_;

    struct mount_point_entry
    {
        std::string mount_point;
        std::filesystem::path base_dir;
        http::fields headers;
    };
    std::vector<mount_point_entry> static_file_entry_;
};
router::router(std::shared_ptr<spdlog::logger> logger) : impl_(new impl(logger)) { }
router::~router()
{
    // delete impl_;
}
bool router::has_handler(http::verb method, std::string_view target) const { return true; }
net::awaitable<void> router::routing(request& req, response& resp)
{
    try
    {
        co_await impl_->proc_routing_befor(req, resp);
        co_await impl_->proc_routing(req, resp);
        co_await impl_->proc_routing_after(req, resp);
    }
    catch (const std::exception& e)
    {
        impl_->logger_->warn("exception in business function, reason: {}", e.what());
        resp.base().result(http::status::internal_server_error);
        resp.base().set(http::field::content_type, "text/html");
        resp.set_body<http::string_body>(e.what());
    }
    catch (...)
    {
        impl_->logger_->warn("unknown exception in business function");
        resp.base().result(http::status::internal_server_error);
        resp.base().set(http::field::content_type, "text/html");
        resp.set_body<http::string_body>("unknown exception");
    }

    resp.base().set(http::field::connection, resp.keep_alive() ? "keep-alive" : "close");
    if (resp.base().result_int() >= 400 && resp.is_body_type<http::empty_body>())
    {
        resp.set_body<http::string_body>(
            html::fromat_error_content(resp.base().result_int(), resp.base().reason(), BOOST_BEAST_VERSION_STRING));
    }
    resp.prepare_payload();
}
bool router::set_mount_point(const std::string& mount_point,
                             const std::filesystem::path& dir,
                             const http::fields& headers /*= {}*/)
{
    if (std::filesystem::is_directory(dir))
    {
        std::string mnt = !mount_point.empty() ? mount_point : "/";
        if (!mnt.empty() && mnt[0] == '/')
        {
            impl_->static_file_entry_.push_back({mnt, dir, headers});
            std::sort(impl_->static_file_entry_.begin(),
                      impl_->static_file_entry_.end(),
                      [](const auto& left, const auto& right)
                      { return left.mount_point.size() > right.mount_point.size(); });
            return true;
        }
    }
    impl_->logger_->warn("set_mount_point path: {} is not directory", dir.string());
    return false;
}
bool router::remove_mount_point(const std::string& mount_point)
{
    for (auto it = impl_->static_file_entry_.begin(); it != impl_->static_file_entry_.end(); ++it)
    {
        if (it->mount_point == mount_point)
        {
            impl_->static_file_entry_.erase(it);
            return true;
        }
    }
    return false;
}

void router::set_http_handler(http::verb method, std::string_view key, coro_http_handler_type&& handler)
{
    auto whole_str = detail::make_whole_str(method, key);

    if (whole_str.find(":") != std::string::npos)
    {
        impl_->coro_router_tree_->coro_insert(whole_str, std::move(handler), method);
        return;
    }

    if (whole_str.find("{") != std::string::npos || whole_str.find(")") != std::string::npos)
    {
        std::string pattern = whole_str;

        if (pattern.find("{}") != std::string::npos)
        {
            boost::replace_all(pattern, "{}", "([^/]+)");
        }

        impl_->coro_regex_handles_.emplace_back(std::regex(pattern), std::move(handler));
        return;
    }
    auto& map = impl_->coro_handles_[std::string(key)];
    if (map.count(method))
    {
        impl_->logger_->warn("router method: {} key: {} has already registered.", http::to_string(method), key);
        return;
    }
    map[method] = std::move(handler);
}


void router::set_default_handler(coro_http_handler_type&& handler) { impl_->default_handler_ = std::move(handler); }

void router::set_file_request_handler(coro_http_handler_type&& handler)
{
    impl_->file_request_handler_ = std::move(handler);
}

} // namespace httplib