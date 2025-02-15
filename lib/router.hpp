#pragma once
#include "radix_tree.hpp"
#include "type_traits.h"
#include <algorithm>
#include <boost/algorithm/string.hpp>

#include <functional>
#include <regex>
#include <set>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <unordered_map>

namespace httplib {
template<class, class = void>
struct has_before : std::false_type {};

template<class T>
struct has_before<T, std::void_t<decltype(std::declval<T>().before(std::declval<request &>(),
                                                                   std::declval<response &>()))>>
    : std::true_type {};

template<class, class = void>
struct has_after : std::false_type {};

template<class T>
struct has_after<T, std::void_t<decltype(std::declval<T>().after(std::declval<request &>(),
                                                                 std::declval<response &>()))>>
    : std::true_type {};

template<class T>
constexpr bool has_before_v = has_before<T>::value;

template<class T>
constexpr bool has_after_v = has_after<T>::value;

template<typename T>
constexpr inline bool is_awaitable_v =
    util::is_specialization_v<std::remove_cvref_t<T>, net::awaitable>;

class router {
public:
    struct mount_point_entry {
        std::string mount_point;
        std::filesystem::path base_dir;
        http::fields headers;
    };

    router(std::shared_ptr<spdlog::logger> logger) : logger_(logger) {}

public:
    static std::string make_whole_str(http::verb method, std::string_view target) {
        return std::format("{} {}", std::string_view(http::to_string(method)), target);
    }
    static std::string make_whole_str(const request &req) {
        return make_whole_str(req.base().method(), req.decoded_target());
    }

public:
    // eg: "GET hello/" as a key
    template<http::verb method, typename Func, typename... Aspects>
    void set_http_handler(std::string_view key, Func handler, Aspects &&...asps) {

        auto whole_str = make_whole_str(method, key);

        // hold keys to make sure map_handles_ key is
        // std::string_view, avoid memcpy when route
        using return_type = typename util::function_traits<Func>::return_type;
        if constexpr (is_awaitable_v<return_type>) {
            coro_http_handler_type http_handler;
            if constexpr (sizeof...(Aspects) > 0) {
                http_handler = [this, handler = std::move(handler),
                                ... asps = std::forward<Aspects>(asps)](
                                   request &req, response &resp) mutable -> net::awaitable<void> {
                    bool ok = true;
                    (do_before(asps, req, resp, ok), ...);
                    if (ok) {
                        co_await handler(req, resp);
                    }
                    ok = true;
                    (do_after(asps, req, resp, ok), ...);
                };
            } else {
                http_handler = std::move(handler);
            }

            if (whole_str.find(":") != std::string::npos) {
                coro_router_tree_->coro_insert(whole_str, std::move(http_handler), method);
            } else {
                if (whole_str.find("{") != std::string::npos ||
                    whole_str.find(")") != std::string::npos) {
                    std::string pattern = whole_str;

                    if (pattern.find("{}") != std::string::npos) {
                        boost::replace_all(pattern, "{}", "([^/]+)");
                    }

                    coro_regex_handles_.emplace_back(std::regex(pattern), std::move(http_handler));
                } else {
                    auto [it, ok] = coro_keys_.emplace(std::move(whole_str));
                    if (!ok) {
                        logger_->warn("router key: {} has already registered.", key);
                        return;
                    }
                    coro_handles_.emplace(*it, std::move(http_handler));
                }
            }
        } else {
            http_handler_type http_handler;
            if constexpr (sizeof...(Aspects) > 0) {
                http_handler = [this, handler = std::move(handler),
                                ... asps = std::forward<Aspects>(asps)](request &req,
                                                                        response &resp) mutable {
                    bool ok = true;
                    (do_before(asps, req, resp, ok), ...);
                    if (ok) {
                        handler(req, resp);
                    }
                    ok = true;
                    (do_after(asps, req, resp, ok), ...);
                };
            } else {
                http_handler = std::move(handler);
            }

            if (whole_str.find(':') != std::string::npos) {
                router_tree_->insert(whole_str, std::move(http_handler), http::to_string(method));
            } else if (whole_str.find("{") != std::string::npos ||
                       whole_str.find(")") != std::string::npos) {
                std::string pattern = whole_str;

                if (pattern.find("{}") != std::string::npos) {
                    boost::replace_all(pattern, "{}", "([^/]+)");
                }

                regex_handles_.emplace_back(std::regex(pattern), std::move(http_handler));
            } else {
                auto [it, ok] = keys_.emplace(std::move(whole_str));
                if (!ok) {
                    logger_->warn("router key: {} has already registered.", key);
                    return;
                }
                map_handles_.emplace(*it, std::move(http_handler));
            }
        }
    }
    bool set_mount_point(const std::string &mount_point, const std::filesystem::path &dir,
                         const http::fields &headers = {}) {
        if (std::filesystem::is_directory(dir)) {
            std::string mnt = !mount_point.empty() ? mount_point : "/";
            if (!mnt.empty() && mnt[0] == '/') {
                static_file_entry_.push_back({mnt, dir, std::move(headers)});
                return true;
            }
        }
        return false;
    }
    bool remove_mount_point(const std::string &mount_point) {
        for (auto it = static_file_entry_.begin(); it != static_file_entry_.end(); ++it) {
            if (it->mount_point == mount_point) {
                static_file_entry_.erase(it);
                return true;
            }
        }
        return false;
    }

    template<typename T>
    void do_before(T &aspect, request &req, response &resp, bool &ok) {
        if constexpr (has_before_v<T>) {
            if (!ok) {
                return;
            }
            ok = aspect.before(req, resp);
        }
    }

    template<typename T>
    void do_after(T &aspect, request &req, response &resp, bool &ok) {
        if constexpr (has_after_v<T>) {
            if (!ok) {
                return;
            }
            ok = aspect.after(req, resp);
        }
    }

    http_handler_type get_handler(std::string_view key) {
        if (auto it = map_handles_.find(key); it != map_handles_.end()) {
            return it->second;
        }
        return nullptr;
    }

    coro_http_handler_type get_coro_handler(std::string_view key) {
        if (auto it = coro_handles_.find(key); it != coro_handles_.end()) {
            return it->second;
        }
        return nullptr;
    }

    const auto &get_handlers() const {
        return map_handles_;
    }

    const auto &get_coro_handlers() const {
        return coro_handles_;
    }

    std::shared_ptr<radix_tree> get_router_tree() {
        return router_tree_;
    }

    std::shared_ptr<radix_tree> get_coro_router_tree() {
        return coro_router_tree_;
    }

    const auto &get_coro_regex_handlers() {
        return coro_regex_handles_;
    }

    const auto &get_regex_handlers() {
        return regex_handles_;
    }
    net::awaitable<void> routing(request &req, response &resp) {

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
        std::tie(is_exist, handler, req.params) =
            get_router_tree()->get(url_path, req.base().method());
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

private:
    inline net::awaitable<bool> handle_file_request(request &req, response &res) {

        if (req.base().method() != http::verb::head && req.base().method() != http::verb::get)
            co_return false;

        for (const auto &entry : static_file_entry_) {
            // Prefix match
            if (!req.decoded_target().compare(0, entry.mount_point.size(), entry.mount_point)) {
                std::string sub_path = req.decoded_target().substr(entry.mount_point.size());
                if (is_valid_path(sub_path)) {
                    auto path = entry.base_dir / sub_path;
                    if (path.string().ends_with('/')) {
                        path += "index.html";
                    }

                    //detail::FileStat stat(path);

                    /*if (std::filesystem::is_directory(path)) {
                        res.set_redirect(sub_path + "/", StatusCode::MovedPermanently_301);
                        co_return true;
                    }*/

                    if (std::filesystem::is_regular_file(path)) {
                        for (const auto &kv : entry.headers) {
                            res.base().set(kv.name(), kv.value());
                        }

                        res.change_body<http::file_body>();
                        beast::error_code ec;
                        res.base().result(http::status::ok);
                        res.body<http::file_body>().open(path.string().c_str(),
                                                         beast::file_mode::read, ec);
                        if (ec)
                            co_return false;

                        if (req.base().method() != http::verb::head && file_request_handler_) {
                            co_await file_request_handler_(req, res);
                        }

                        co_return true;
                    }
                }
            }
        }
        co_return false;
    }
    inline static bool is_valid_path(const std::string &path) {
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

private:
    std::shared_ptr<spdlog::logger> logger_;
    std::set<std::string> keys_;
    std::unordered_map<std::string_view, http_handler_type> map_handles_;

    std::set<std::string> coro_keys_;
    std::unordered_map<std::string_view, coro_http_handler_type> coro_handles_;

    std::shared_ptr<radix_tree> router_tree_ = std::make_shared<radix_tree>(radix_tree());

    std::shared_ptr<radix_tree> coro_router_tree_ = std::make_shared<radix_tree>(radix_tree());

    std::vector<std::tuple<std::regex, http_handler_type>> regex_handles_;

    std::vector<std::tuple<std::regex, coro_http_handler_type>> coro_regex_handles_;

    coro_http_handler_type default_handler_;
    coro_http_handler_type file_request_handler_;

    std::vector<mount_point_entry> static_file_entry_;
};
} // namespace httplib