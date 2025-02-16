#include "httplib/router.h"
#include "httplib/html.h"

namespace httplib {

namespace detail {
inline static bool is_valid_path(std::string_view path) {
    size_t level = 0;
    size_t i = 0;

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
            if (level == 0) {
                return false;
            }
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
} // namespace detail

net::awaitable<void> router::routing(request &req, response &resp) {
    try {
        co_await proc_routing_befor(req, resp);
        co_await proc_routing(req, resp);
        co_await proc_routing_after(req, resp);
    } catch (const std::exception &e) {
        logger_->warn("exception in business function, reason: {}", e.what());
        resp.base().result(http::status::internal_server_error);
        resp.base().set(http::field::content_type, "text/html");
        resp.set_body<http::string_body>(e.what());
    } catch (...) {
        logger_->warn("unknown exception in business function");
        resp.base().result(http::status::internal_server_error);
        resp.base().set(http::field::content_type, "text/html");
        resp.set_body<http::string_body>("unknown exception");
    }

    resp.base().set(http::field::connection, resp.keep_alive() ? "keep-alive" : "close");
    if (resp.base().result_int() >= 400 && resp.is_body_type<http::empty_body>()) {
        resp.set_body<http::string_body>(html::fromat_error_content(
            resp.base().result_int(), resp.base().reason(), BOOST_BEAST_VERSION_STRING));
    }
    resp.prepare_payload();
}

net::awaitable<bool> router::handle_file_request(request &req, response &res) {
    switch (req.method()) {
    case http::verb::head:
    case http::verb::get: {
        std::string decoded_target = req.decoded_target();
        std::string_view target(decoded_target);
        beast::error_code ec;

        for (const auto &entry : static_file_entry_) {
            // Prefix match
            if (!target.starts_with(entry.mount_point))
                continue;
            target.remove_prefix(entry.mount_point.size());
            if (!detail::is_valid_path(target))
                continue;

            auto path = entry.base_dir / std::filesystem::u8path(target);
            if (!std::filesystem::exists(path, ec))
                continue;

            if (std::filesystem::is_directory(path, ec)) {
                if (auto html_path = path / "index.html";
                    std::filesystem::is_regular_file(html_path, ec)) {
                    path = html_path;
                } else if (auto htm_path = path / "index.htm";
                           std::filesystem::is_regular_file(htm_path, ec)) {
                    path = htm_path;
                }
            }

            if (std::filesystem::is_directory(path, ec)) {
                beast::error_code ec;
                auto body = html::format_dir_to_html(req.target(), path, ec);
                if (ec)
                    co_return false;
                res.set_string_content(body, "text/html");
                co_return true;
            }

            if (std::filesystem::is_regular_file(path, ec)) {
                for (const auto &kv : entry.headers) {
                    res.base().set(kv.name(), kv.value());
                }
                beast::error_code ec;
                res.set_file_content(path, ec);
                if (ec)
                    co_return false;

                if (req.method() != http::verb::head && file_request_handler_) {
                    co_await file_request_handler_(req, res);
                }

                co_return true;
            }
        }
    } break;
    default:
        break;
    }
    co_return false;
}

net::awaitable<void> router::proc_routing(request &req, response &resp) {
    if (co_await handle_file_request(req, resp))
        co_return;
    auto key = router::make_whole_str(req);

    if (auto handler = get_handler(key); handler) {
        handler(req, resp);
        co_return;
    }
    if (auto coro_handler = get_coro_handler(key); coro_handler) {
        co_await coro_handler(req, resp);
        co_return;
    }
    if (default_handler_) {
        co_await default_handler_(req, resp);
        co_return;
    }

    bool is_exist = false;

    http_handler_type handler;
    std::string url_path = router::make_whole_str(req.base().method(), req.base().target());
    std::tie(is_exist, handler, req.params) = get_router_tree()->get(url_path, req.base().method());
    if (is_exist) {
        if (handler) {
            (handler)(req, resp);
        } else {
            resp.base().result(http::status::not_found);
        }
        co_return;
    }

    bool is_coro_exist = false;
    coro_http_handler_type coro_handler;
    std::tie(is_coro_exist, coro_handler, req.params) =
        get_coro_router_tree()->get_coro(url_path, req.base().method());

    if (is_coro_exist) {
        if (coro_handler) {
            co_await coro_handler(req, resp);
        } else {
            resp.base().result(http::status::not_found);
        }
        co_return;
    }
    bool is_matched_regex_router = false;
    // coro regex router
    const auto &coro_regex_handlers = get_coro_regex_handlers();
    if (coro_regex_handlers.size() != 0) {
        for (auto &pair : coro_regex_handlers) {
            std::string coro_regex_key{key};

            if (std::regex_match(coro_regex_key, req.matches, std::get<0>(pair))) {
                auto coro_handler = std::get<1>(pair);
                if (coro_handler) {
                    co_await coro_handler(req, resp);
                    is_matched_regex_router = true;
                }
            }
        }
    }
    // regex router
    if (!is_matched_regex_router) {
        const auto &regex_handlers = get_regex_handlers();
        if (regex_handlers.size() != 0) {
            for (auto &pair : regex_handlers) {
                std::string regex_key{key};
                if (std::regex_match(regex_key, req.matches, std::get<0>(pair))) {
                    auto handler = std::get<1>(pair);
                    if (handler) {
                        (handler)(req, resp);
                        is_matched_regex_router = true;
                    }
                }
            }
        }
    }
    // not found
    if (!is_matched_regex_router) {
        resp.base().result(http::status::not_found);
    }
    co_return;
}

bool router::set_mount_point(const std::string &mount_point, const std::filesystem::path &dir,
                             const http::fields &headers /*= {}*/) {
    if (std::filesystem::is_directory(dir)) {
        std::string mnt = !mount_point.empty() ? mount_point : "/";
        if (!mnt.empty() && mnt[0] == '/') {
            static_file_entry_.push_back({mnt, dir, headers});
            std::sort(static_file_entry_.begin(), static_file_entry_.end(),
                      std::greater<mount_point_entry>());
            return true;
        }
    }
    return false;
}

bool router::remove_mount_point(const std::string &mount_point) {
    for (auto it = static_file_entry_.begin(); it != static_file_entry_.end(); ++it) {
        if (it->mount_point == mount_point) {
            static_file_entry_.erase(it);
            return true;
        }
    }
    return false;
}

} // namespace httplib