#pragma once
#include "radix_tree.hpp"
#include "type_traits.h"
#include <algorithm>
#include <functional>
#include <regex>
#include <set>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <boost/algorithm/string.hpp>

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
    router(std::shared_ptr<spdlog::logger> logger) : logger_(logger) {}

public:
    // eg: "GET hello/" as a key
    template<http::verb method, typename Func, typename... Aspects>
    void set_http_handler(std::string key, Func handler, Aspects &&...asps) {
        auto method_name = http::to_string(method);
        std::string whole_str;
        whole_str.append(method_name).append(" ").append(key);

        // hold keys to make sure map_handles_ key is
        // std::string_view, avoid memcpy when route
        using return_type = typename util::function_traits<Func>::return_type;
        if constexpr (is_awaitable_v<return_type>) {
            std::function<net::awaitable<void>(request & req, response & resp)> http_handler;
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
                std::string method_str(method_name);
                coro_router_tree_->coro_insert(whole_str, std::move(http_handler), method_str);
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
            std::function<void(request & req, response & resp)> http_handler;
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
                std::string method_str(method_name);
                router_tree_->insert(whole_str, std::move(http_handler), method_str);
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

    std::function<void(request &req, response &resp)> *get_handler(std::string_view key) {
        if (auto it = map_handles_.find(key); it != map_handles_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    std::function<net::awaitable<void>(request &req, response &resp)> *
    get_coro_handler(std::string_view key) {
        if (auto it = coro_handles_.find(key); it != coro_handles_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    void route(auto handler, request &req, response &resp, std::string_view key) {
        try {
            (*handler)(req, resp);
        } catch (const std::exception &e) {
            logger_->warn("exception in business function, reason: {}", e.what());
            resp.base().result(http::status::internal_server_error);
        } catch (...) {
            logger_->warn("unknown exception in business function");
            resp.base().result(http::status::internal_server_error);
        }
    }

    net::awaitable<void> route_coro(auto handler, request &req, response &resp,
                                    std::string_view key) {
        try {
            co_await (*handler)(req, resp);
        } catch (const std::exception &e) {
            logger_->warn("exception in business function, reason: {}", e.what());
            resp.base().result(http::status::internal_server_error);
        } catch (...) {
            logger_->warn("unknown exception in business function");
            resp.base().result(http::status::internal_server_error);
        }
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

private:
    std::shared_ptr<spdlog::logger> logger_;
    std::set<std::string> keys_;
    std::unordered_map<std::string_view, std::function<void(request &req, response &resp)>>
        map_handles_;

    std::set<std::string> coro_keys_;
    std::unordered_map<std::string_view,
                       std::function<net::awaitable<void>(request &req, response &resp)>>
        coro_handles_;

    std::shared_ptr<radix_tree> router_tree_ = std::make_shared<radix_tree>(radix_tree());

    std::shared_ptr<radix_tree> coro_router_tree_ = std::make_shared<radix_tree>(radix_tree());

    std::vector<std::tuple<std::regex, std::function<void(request &req, response &resp)>>>
        regex_handles_;

    std::vector<
        std::tuple<std::regex, std::function<net::awaitable<void>(request &req, response &resp)>>>
        coro_regex_handles_;
};
} // namespace httplib