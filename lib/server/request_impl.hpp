#pragma once
#include "body/any_body.hpp"
#include "httplib/server/request.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "stream/http_stream.hpp"
#include <algorithm>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/system/result.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>

namespace httplib::server
{

    class request::impl : public http::request<body::any_body>
    {
      public:
        impl(tcp::endpoint const& local_endpoint,
             tcp::endpoint const& remote_endpoint,
             http::request<body::any_body>&& other)
            : http::request<body::any_body>(std::move(other))
            , local_endpoint_(local_endpoint)
            , remote_endpoint_(remote_endpoint)
        {
            if (auto pos = this->target().find("?"); pos == std::string_view::npos)
            {
                this->decoded_path_ = util::url_decode(this->target());
            }
            else
            {
                this->decoded_path_ = util::url_decode(this->target().substr(0, pos));
                this->query_params_.decode(this->target().substr(pos + 1));
            }
        }

        impl(tcp::endpoint const& local_endpoint,
             tcp::endpoint const& remote_endpoint,
             http::request<http::empty_body>&& other)
            : impl(local_endpoint, remote_endpoint, http::request<body::any_body>(other))
        {
        }

        impl& operator=(impl&& other) noexcept = default;
        impl(impl&& other) noexcept = default;

        std::string_view
        path() const
        {
            if (this->decoded_path_.empty())
            {
                return std::string_view(this->target());
            }

            return this->decoded_path_;
        }
        html::query_params const&
        query_params() const
        {
            return query_params_;
        }

        net::ip::address
        get_client_ip() const
        {
            auto iter = this->find("X-Forwarded-For");
            if (iter == this->end())
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

        // ---- lazy body reader（对照 client::response::impl）----

        using body_setup_fn = std::function<void(http::request<body::any_body>&)>;

        struct lazy_body_read_ctx
        {
            http_stream* stream = nullptr;
            beast::flat_buffer* buffer = nullptr;
            std::chrono::steady_clock::duration read_timeout { 30 };
            std::unique_ptr<http::request_parser<http::empty_body>> header_parser;
            std::unique_ptr<http::request_parser<http::buffer_body>> resp_parser;
            // 解压流式读取专用：any_body 解析器，body 持有 buffer_body 值以走解压 reader。
            std::unique_ptr<http::request_parser<body::any_body>> dec_parser;
            // multipart/form-data 落盘配置（默认关闭）。
            fs::path upload_dir;
            std::uint64_t upload_file_limit = 0;
        };

        // 构造 lazy 请求（body 未读）：header 已解析完毕，保留 header_parser 供后续读取。
        void
        setup_lazy_reading(http_stream& stream,
                           beast::flat_buffer& buffer,
                           std::unique_ptr<http::request_parser<http::empty_body>> header_parser,
                           std::chrono::steady_clock::duration read_timeout,
                           fs::path upload_dir = {},
                           std::uint64_t upload_file_limit = 0)
        {
            lazy_ctx_ = std::make_unique<lazy_body_read_ctx>();
            lazy_ctx_->stream = &stream;
            lazy_ctx_->buffer = &buffer;
            lazy_ctx_->read_timeout = read_timeout;
            lazy_ctx_->header_parser = std::move(header_parser);
            lazy_ctx_->upload_dir = std::move(upload_dir);
            lazy_ctx_->upload_file_limit = upload_file_limit;
        }

        bool
        is_lazy() const
        {
            return lazy_ctx_ != nullptr;
        }

        bool
        is_body_done() const
        {
            if (!lazy_ctx_)
            {
                return true;
            }
            auto& ctx = *lazy_ctx_;
            if (ctx.resp_parser)
            {
                return ctx.resp_parser->is_done();
            }
            if (ctx.dec_parser)
            {
                if (!ctx.dec_parser->is_done())
                {
                    return false;
                }
                // 解析器已读完，但解压溢出数据可能还没取完。
                if (auto const* buf_body = std::get_if<body::buffer_body::value_type>(&ctx.dec_parser->get().body()))
                {
                    return buf_body->pending.empty();
                }
                return true;
            }
            // 尚未开始读取：header 之后若还有 body 则未读完。
            return !ctx.header_parser || ctx.header_parser->is_done();
        }

        // 流式读取原始（未解压）body：把 header_parser 转成 buffer_body 解析器。
        net::awaitable<boost::system::result<std::size_t>>
        read_some_raw(net::mutable_buffer const& buf)
        {
            if (!lazy_ctx_)
            {
                co_return 0;
            }
            auto& ctx = *lazy_ctx_;
            if (!ctx.resp_parser)
            {
                if (!ctx.header_parser)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
                }
                ctx.resp_parser = std::make_unique<http::request_parser<http::buffer_body>>(std::move(*ctx.header_parser));
                ctx.resp_parser->eager(true);
                ctx.header_parser.reset();
            }

            for (;;)
            {
                if (ctx.resp_parser->is_done())
                {
                    co_return 0;
                }

                auto& body = ctx.resp_parser->get().body();
                body.data = (void*)buf.data();
                body.size = buf.size();

                boost::system::error_code ec;
                ctx.stream->expires_after(ctx.read_timeout);
                co_await http::async_read_some(*ctx.stream, *ctx.buffer, *ctx.resp_parser, util::net_awaitable[ec]);
                ctx.stream->expires_never();
                if (ec == http::error::need_buffer)
                {
                    ec = {};
                }
                if (ec)
                {
                    co_return ec;
                }

                auto consumed = buf.size() - body.size;
                if (consumed > 0)
                {
                    co_return consumed;
                }

                if (ctx.resp_parser->is_done())
                {
                    co_return 0;
                }
            }
        }

        // 流式读取解压后的 body：把 buffer_body 放进 any_body，复用其 content-encoding 解压逻辑。
        // 调用方缓冲写不下时溢出到 value_type::pending，下次调用先取 pending，保证不丢数据。
        net::awaitable<boost::system::result<std::size_t>>
        read_some_decompressed(net::mutable_buffer const& buf)
        {
            if (!lazy_ctx_)
            {
                co_return 0;
            }
            if (buf.size() == 0)
            {
                co_return 0;
            }
            auto& ctx = *lazy_ctx_;
            if (!ctx.dec_parser)
            {
                if (!ctx.header_parser)
                {
                    co_return boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
                }
                ctx.dec_parser = std::make_unique<http::request_parser<body::any_body>>(std::move(*ctx.header_parser));
                ctx.dec_parser->eager(true);
                ctx.header_parser.reset();
                ctx.dec_parser->get().body() = body::buffer_body::value_type {};
            }

            for (;;)
            {
                auto& buf_body = std::get<body::buffer_body::value_type>(ctx.dec_parser->get().body());

                // 先取上一次没写完的溢出数据。
                if (!buf_body.pending.empty())
                {
                    auto n = std::min(buf.size(), buf_body.pending.size());
                    std::memcpy(buf.data(), buf_body.pending.data(), n);
                    buf_body.pending.erase(0, n);
                    co_return n;
                }

                if (ctx.dec_parser->is_done())
                {
                    co_return 0;
                }

                buf_body.data = (void*)buf.data();
                buf_body.size = buf.size();

                boost::system::error_code ec;
                ctx.stream->expires_after(ctx.read_timeout);
                co_await http::async_read_some(*ctx.stream, *ctx.buffer, *ctx.dec_parser, util::net_awaitable[ec]);
                ctx.stream->expires_never();
                if (ec == http::error::need_buffer)
                {
                    ec = {};
                }
                if (ec)
                {
                    co_return ec;
                }

                auto consumed = buf.size() - buf_body.size;
                if (consumed > 0)
                {
                    co_return consumed;
                }

                if (ctx.dec_parser->is_done())
                {
                    co_return 0;
                }
            }
        }

        // 读取剩余 body 并按 body_setup 物化到本请求。
        net::awaitable<boost::system::error_code>
        read_body(body_setup_fn const& body_setup)
        {
            if (!lazy_ctx_)
            {
                co_return boost::system::error_code {};
            }
            auto& ctx = *lazy_ctx_;
            if (!ctx.header_parser)
            {
                // 已物化完成则视为成功；已转流式读取则拒绝。
                if (is_body_done())
                {
                    co_return boost::system::error_code {};
                }
                co_return boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
            }

            http::request_parser<body::any_body> body_parser(std::move(*ctx.header_parser));
            body_parser.eager(true);
            ctx.header_parser.reset();

            if (body_setup)
            {
                body_setup(body_parser.get());
            }
            if (!ctx.upload_dir.empty())
            {
                auto& msg = body_parser.get();
                if (msg[http::field::content_type].starts_with("multipart/form-data"))
                {
                    auto& body = msg.body();
                    if (!std::holds_alternative<body::form_data_body::value_type>(body))
                    {
                        body = body::form_data_body::value_type {};
                    }
                    auto& fd = std::get<body::form_data_body::value_type>(body);
                    fd.save_dir = ctx.upload_dir;
                    fd.max_file_size = ctx.upload_file_limit;
                }
            }

            while (!body_parser.is_done())
            {
                boost::system::error_code ec;
                ctx.stream->expires_after(ctx.read_timeout);
                co_await http::async_read_some(*ctx.stream, *ctx.buffer, body_parser, util::net_awaitable[ec]);
                ctx.stream->expires_never();
                if (ec)
                {
                    co_return ec;
                }
            }
            this->body() = std::move(body_parser.release().body());
            co_return boost::system::error_code {};
        }

        std::string_view
        operator[](http::field name) const
        {
            return this->base()[name];
        }
        std::string_view
        operator[](std::string_view name) const
        {
            return this->base()[name];
        }
        std::string_view
        at(http::field name) const
        {
            return this->base().at(name);
        }
        std::string_view
        at(std::string_view name) const
        {
            return this->base().at(name);
        }

        bool
        has(http::field name) const
        {
            return this->base().find(name) != this->base().end();
        }
        bool
        has(std::string_view name) const
        {
            return this->base().find(name) != this->base().end();
        }
        void
        set(http::field name, std::string_view value)
        {
            this->base().set(name, value);
        }
        void
        set(std::string_view name, std::string_view value)
        {
            this->base().set(name, value);
        }
        void
        erase(http::field name)
        {
            this->base().erase(name);
        }
        void
        erase(std::string_view name)
        {
            this->base().erase(name);
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

        static request
        make_request(tcp::endpoint const& local_endpoint,
                     tcp::endpoint const& remote_endpoint,
                     http::request<body::any_body>&& other)
        {
            auto _impl = std::make_unique<request::impl>(local_endpoint, remote_endpoint, std::move(other));
            return request(std::move(_impl));
        }
        static request
        make_request(tcp::endpoint const& local_endpoint,
                     tcp::endpoint const& remote_endpoint,
                     http::request<http::empty_body>&& other)
        {
            auto _impl = std::make_unique<request::impl>(local_endpoint, remote_endpoint, std::move(other));
            return request(std::move(_impl));
        }

      private:
        std::string decoded_path_;
        html::query_params query_params_;

        tcp::endpoint local_endpoint_;
        tcp::endpoint remote_endpoint_;

        std::unordered_map<std::string, std::string> path_params_;
        request_data data_;

        // ---- lazy body reader state ----
        std::unique_ptr<lazy_body_read_ctx> lazy_ctx_;
    };
} // namespace httplib::server
