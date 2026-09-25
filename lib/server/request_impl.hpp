#pragma once
#include "body/body_reader.hpp"
#include "body/body_state.hpp"
#include "body/sink.hpp"
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

    class request::impl
    {
      public:
        using body_reader_t = httplib::detail::body_reader<true, session::http_task>;

        impl(tcp::endpoint const& local_endpoint,
             tcp::endpoint const& remote_endpoint,
             std::unique_ptr<http::request_parser<http::empty_body>> header_parser,
             std::shared_ptr<session::http_task> task,
             bool is_ssl)
            : header_(header_parser->get().base())
            , keep_alive_(header_parser->get().keep_alive())
            , local_endpoint_(local_endpoint)
            , remote_endpoint_(remote_endpoint)
            , is_ssl_(is_ssl)
            , reader_(std::move(task))
            , body_reader_(reader_->executor(), reader_.get(), std::move(*header_parser), reader_->body_limit())
        {
            if (auto pos = header_.target().find("?"); pos == std::string_view::npos)
            {
                decoded_path_ = url::url_decode(header_.target());
            }
            else
            {
                decoded_path_ = url::url_decode(header_.target().substr(0, pos));
                query_params_.decode(header_.target().substr(pos + 1));
            }
            body_reader_.set_form_data_params(reader_->form_data_params());
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
        httplib::query_params const&
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
        // ---- body reader ----

        body_reader_t&
        reader()
        {
            return body_reader_;
        }

        body_reader_t const&
        reader() const
        {
            return body_reader_;
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
        httplib::query_params query_params_;

        tcp::endpoint local_endpoint_;
        tcp::endpoint remote_endpoint_;
        bool is_ssl_ = false;

        std::unordered_map<std::string, std::string> path_params_;
        request_data data_;

        // ---- lazy body reader state ----
        // 连接所有者（http_task），作用同 client 的 parent_（http_client::impl）。
        // 共享所有权：请求对其所依赖的连接读取栈保持强引用，避免裸指针悬空。
        std::shared_ptr<session::http_task> reader_;

        // 读取栈：解析器状态 + 串行化 + 物化后的 body（内部持 body_state）。
        // 声明在 reader_ 之后，保证析构顺序上先于 reader_。
        body_reader_t body_reader_;
    };
} // namespace httplib::server
