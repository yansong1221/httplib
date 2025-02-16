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

        bool operator<(const mount_point_entry &other) const {
            return mount_point.size() < mount_point.size();
        }
        bool operator=(const mount_point_entry &other) const {
            return mount_point.size() == mount_point.size();
        }
        bool operator>(const mount_point_entry &other) const {
            return mount_point.size() > mount_point.size();
        }
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
                         const http::fields &headers = {});

    bool remove_mount_point(const std::string &mount_point);

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

    net::awaitable<void> routing(request &req, response &resp);

private:
    net::awaitable<void> proc_routing_befor(request &req, response &resp) {
        co_return;
    }
    net::awaitable<void> proc_routing(request &req, response &resp);
    net::awaitable<void> proc_routing_after(request &req, response &resp) {
        co_return;
    }

private:
    inline net::awaitable<bool> handle_file_request(request &req, response &res);

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