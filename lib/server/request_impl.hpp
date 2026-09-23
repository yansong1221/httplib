#pragma once
#include "body/any_body.hpp"
#include "body/lazy_body_reader.hpp"
#include "httplib/server/request.hpp"
#include "httplib/url/url.hpp"
#include "httplib/util/misc.hpp"
#include "session.hpp"
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

    class request::impl : public httplib::detail::lazy_body_reader<true, request::impl>
    {
        friend class httplib::detail::lazy_body_reader<true, request::impl>;

        using lazy_reader = httplib::detail::lazy_body_reader<true, request::impl>;

      public:
        impl(tcp::endpoint const& local_endpoint,
             tcp::endpoint const& remote_endpoint,
             std::unique_ptr<http::request_parser<http::empty_body>> header_parser,
             std::shared_ptr<session::http_task> task,
             bool is_ssl)
            : local_endpoint_(local_endpoint)
            , remote_endpoint_(remote_endpoint)
            , is_ssl_(is_ssl)
            , reader_(std::move(task))
        {
            header_ = header_parser->get().base();
            keep_alive_ = header_parser->get().keep_alive();

            if (auto pos = header_.target().find("?"); pos == std::string_view::npos)
            {
                decoded_path_ = url::url_decode(header_.target());
            }
            else
            {
                decoded_path_ = url::url_decode(header_.target().substr(0, pos));
                query_params_.decode(header_.target().substr(pos + 1));
            }
            start(std::move(header_parser), reader_->body_limit(), reader_->executor());
        }

        impl& operator=(impl&& other) noexcept = default;
        impl(impl&& other) noexcept = default;

        bool
        keep_alive() const
        {
            return keep_alive_;
        }

        std::string_view
        path() const
        {
            if (decoded_path_.empty())
            {
                return header_.target();
            }
            return decoded_path_;
        }
        html::query_params const&
        query_params() const
        {
            return query_params_;
        }

        net::ip::address
        get_client_ip() const
        {
            auto iter = header_.find("X-Forwarded-For");
            if (iter == header_.end())
            {
                return this->remote_endpoint_.address();
            }

            auto tokens = util::split(iter->value(), ",");
            if (tokens.empty())
            {
                return this->remote_endpoint_.address();
            }

            boost::system::error_code ec;
            auto address = net::ip::make_address(tokens.front(), ec);
            if (ec)
            {
                return this->remote_endpoint_.address();
            }
            return address;
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
        // ---- eager accessors ----

        std::string const&
        as_string() const
        {
            if (!msg_)
            {
                throw std::bad_variant_access {};
            }
            return std::get<std::string>(msg_->body());
        }

        boost::json::value const&
        as_json() const
        {
            if (!msg_)
            {
                throw std::bad_variant_access {};
            }
            return std::get<boost::json::value>(msg_->body());
        }

        html::form_data const&
        as_form_data() const
        {
            if (!msg_)
            {
                throw std::bad_variant_access {};
            }
            return std::get<html::form_data>(msg_->body());
        }

        html::query_params const&
        as_query_params() const
        {
            if (!msg_)
            {
                throw std::bad_variant_access {};
            }
            return std::get<html::query_params>(msg_->body());
        }

        bool
        is_empty() const
        {
            return msg_ && msg_->body().template is_body_type<body::empty_body>();
        }

        bool
        is_string() const
        {
            return msg_ && msg_->body().template is_body_type<body::string_body>();
        }

        bool
        is_json() const
        {
            return msg_ && msg_->body().template is_body_type<body::json_body>();
        }

        bool
        is_form_data() const
        {
            return msg_ && msg_->body().template is_body_type<body::form_data_body>();
        }

        bool
        is_query_params() const
        {
            return msg_ && msg_->body().template is_body_type<body::query_params_body>();
        }

        bool
        is_body_done()
        {
            if (msg_)
            {
                return true;
            }
            return lazy_reader::is_body_done();
        }

        html::form_data::param
        form_data_params() const
        {
            return reader_->form_data_params();
        }
        http::request_header<http::fields>&
        header()
        {
            return header_;
        }
        http::request_header<http::fields> const&
        header() const
        {
            return header_;
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
        set_path_param(std::unordered_map<std::string, std::string>&& params)
        {
            path_params_ = std::move(params);
        }

        // 移动取出已物化的 body（不拷贝，取出后本响应不再持有该 body）。
        template <typename T>
        T
        take_body()
        {
            return std::move(std::get<T>(msg_->body()));
        }

        // 常规请求：header_parser 归入 lazy 读取栈，随时可流式读取或全量物化
        // （http_task 提供连接读取接口，作用同 client 的 parent_）。
        static request
        make_request(std::shared_ptr<session::http_task> task,
                     tcp::endpoint const& local_endpoint,
                     tcp::endpoint const& remote_endpoint,
                     std::unique_ptr<http::request_parser<http::empty_body>> header_parser,
                     bool is_ssl = false)
        {
            auto _impl = std::make_unique<request::impl>(local_endpoint,
                                                         remote_endpoint,
                                                         std::move(header_parser),
                                                         std::move(task),
                                                         is_ssl);
            return request(std::move(_impl));
        }

      private:
        http::request_header<http::fields> header_;
        bool keep_alive_;

        std::string decoded_path_;
        html::query_params query_params_;

        tcp::endpoint local_endpoint_;
        tcp::endpoint remote_endpoint_;
        bool is_ssl_ = false;

        std::unordered_map<std::string, std::string> path_params_;
        request_data data_;

        // ---- lazy body reader state ----
        // 连接所有者（http_task），作用同 client 的 parent_（http_client::impl）。
        // 共享所有权：请求对其所依赖的连接读取栈保持强引用，避免裸指针悬空。
        std::shared_ptr<session::http_task> reader_;

        std::optional<http::request<body::any_body>> msg_;

        // ---- httplib::detail::lazy_body_reader data source / materialization ----
        template <typename Parser>
        net::awaitable<void>
        read_some(Parser& parser, boost::system::error_code& ec)
        {
            co_await reader_->read_some(parser, ec);
        }
        bool
        reader_is_materialized() const
        {
            // 非 lazy 请求的 body 已在 session 阶段物化到本请求。
            return reader_ == nullptr;
        }
        void
        store_body(http::request<body::any_body>&& msg)
        {
            msg_ = std::move(msg);
        }
    };
} // namespace httplib::server
