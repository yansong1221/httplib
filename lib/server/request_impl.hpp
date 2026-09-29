#pragma once
#include "beast_alias.hpp"
#include "body/body_reader.hpp"
#include "body/sink.hpp"
#include "httplib/server/request.hpp"
#include "httplib/url/url.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/util/string_hash.hpp"
#include "session.hpp"
#include "trusted_proxies.hpp"
#include <algorithm>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/system/result.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>

namespace httplib::server
{

    class request::impl : public httplib::detail::body_reader<true, session::http_task>
    {
      public:
        using body_reader_t = httplib::detail::body_reader<true, session::http_task>;

        impl(tcp::endpoint const& local_endpoint,
             tcp::endpoint const& remote_endpoint,
             std::unique_ptr<http::request_parser<http::empty_body>> header_parser,
             std::shared_ptr<session::http_task> task,
             bool is_ssl,
             std::shared_ptr<trusted_proxies const> trusted_proxies = {})
            : body_reader_t(task->executor(), task.get(), std::move(*header_parser), task->body_limit())
            , local_endpoint_(local_endpoint)
            , remote_endpoint_(remote_endpoint)
            , is_ssl_(is_ssl)
            , trusted_proxies_(std::move(trusted_proxies))
            , task_(std::move(task))
        {
            auto const target = get().target();
            if (auto pos = target.find("?"); pos == std::string_view::npos)
            {
                decoded_path_ = url::url_decode(target);
            }
            else
            {
                decoded_path_ = url::url_decode(target.substr(0, pos));
                query_params_.decode(target.substr(pos + 1));
            }
            set_form_data_params(task_->form_data_params());
        }

        impl& operator=(impl&& other) noexcept = default;
        impl(impl&& other) noexcept = default;

        std::string_view
        path() const
        {
            if (decoded_path_.empty())
            {
                return get().target();
            }
            return decoded_path_;
        }
        httplib::query_params const&
        query_params() const
        {
            return query_params_;
        }

        net::ip::address
        get_client_ip() const
        {
            auto const peer = remote_endpoint_.address();

            // 直连对端不在可信集合里：X-Forwarded-For 完全由客户端填写，
            // 采信它等于让任何人自己声明 IP（限流、日志、审计全部可伪造）。
            if (!trusted_proxies_ || !trusted_proxies_->contains(peer))
            {
                return peer;
            }

            auto iter = get().find("X-Forwarded-For");
            if (iter == get().end())
            {
                return peer;
            }

            // X-Forwarded-For 由每一跳代理追加（见 reverse_proxy_impl.cpp），因此
            // 最右侧是离本机最近的一跳。从右往左跳过可信代理本身，取第一个非可信
            // 地址——它才是真正的客户端。取最左端（最常见的错误写法）会被客户端
            // 预置伪造值直接骗过。
            auto tokens = util::split(iter->value(), ",");
            for (auto it = tokens.rbegin(); it != tokens.rend(); ++it)
            {
                boost::system::error_code ec;
                auto const addr = net::ip::make_address(*it, ec);
                if (ec)
                {
                    // 无法解析的项直接跳过，继续往左找；全都不合法则退回对端地址。
                    continue;
                }
                if (!trusted_proxies_->contains(addr))
                {
                    return addr;
                }
            }
            return peer;
        }
        tcp::endpoint const&
        local_endpoint() const
        {
            return this->local_endpoint_;
        }
        tcp::endpoint const&
        remote_endpoint() const
        {
            return this->remote_endpoint_;
        }
        bool
        is_ssl() const
        {
            return is_ssl_;
        }

        request_data&
        data()
        {
            return data_;
        }
        request_data const&
        data() const
        {
            return data_;
        }

        std::string_view
        path_param(std::string const& key) const
        {
            auto it = path_params_.find(key);
            if (it != path_params_.end())
            {
                return it->second;
            }
            return {};
        }
        void
        set_path_param(std::string const& key, std::string const& val)
        {
            path_params_[key] = val;
        }
        void
        set_path_param(util::string_map<std::string>&& params)
        {
            path_params_ = std::move(params);
        }

        // 常规请求：header_parser 归入 lazy 读取栈，随时可流式读取或全量物化
        // （http_task 提供连接读取接口，作用同 client 的 parent_）。
        static request
        make_request(std::shared_ptr<session::http_task> task,
                     tcp::endpoint const& local_endpoint,
                     tcp::endpoint const& remote_endpoint,
                     std::unique_ptr<http::request_parser<http::empty_body>> header_parser,
                     bool is_ssl = false,
                     std::shared_ptr<trusted_proxies const> trusted_proxies = {})
        {
            auto _impl = std::make_unique<request::impl>(local_endpoint,
                                                         remote_endpoint,
                                                         std::move(header_parser),
                                                         std::move(task),
                                                         is_ssl,
                                                         std::move(trusted_proxies));
            return request(std::move(_impl));
        }

      private:
        std::string decoded_path_;
        httplib::query_params query_params_;

        tcp::endpoint local_endpoint_;
        tcp::endpoint remote_endpoint_;
        bool is_ssl_ = false;

        /// server 的可信代理配置快照（默认空 = 不采信任何 XFF）。不可变，跨线程
        /// 只读，无需加锁。
        std::shared_ptr<trusted_proxies const> trusted_proxies_;

        util::string_map<std::string> path_params_;
        request_data data_;

        // 连接所有者（http_task），作用同 client 的 parent_（http_client::impl）。
        // 共享所有权：请求对其所依赖的连接读取栈保持强引用，避免裸指针悬空。
        // body_reader 基类的 source_ 指向本对象；其析构不访问 source_，故成员先于基类
        // 析构（task_ 先释放）是安全的。
        std::shared_ptr<session::http_task> task_;
    };
} // namespace httplib::server
