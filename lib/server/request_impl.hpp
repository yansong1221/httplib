#pragma once
#include "httplib/server/middleware/session.hpp"
#include "httplib/server/request.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "stream/http_stream.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <chrono>
#include <deque>
#include <functional>

namespace httplib::server {

class request::impl : public http::request<body::any_body>
{
public:
    impl(const tcp::endpoint& local_endpoint,
         const tcp::endpoint& remote_endpoint,
         http::request<body::any_body>&& other)
        : http::request<body::any_body>(std::move(other))
        , local_endpoint_(local_endpoint)
        , remote_endpoint_(remote_endpoint)
    {
        if (auto pos = this->target().find("?"); pos == std::string_view::npos) {
            this->decoded_path_ = util::url_decode(this->target());
        }
        else {
            this->decoded_path_ = util::url_decode(this->target().substr(0, pos));
            this->query_params_.decode(this->target().substr(pos + 1));
        }
    }

    impl(const tcp::endpoint& local_endpoint,
         const tcp::endpoint& remote_endpoint,
         http::request<http::empty_body>&& other)
        : impl(local_endpoint, remote_endpoint, http::request<body::any_body>(other))
    {
    }

    impl& operator=(impl&& other) noexcept = default;
    impl(impl&& other) noexcept = default;

    std::string_view path() const
    {
        if (this->decoded_path_.empty())
            return std::string_view(this->target());

        return this->decoded_path_;
    }
    const html::query_params& query_params() const { return query_params_; }

    net::ip::address get_client_ip() const
    {
        auto iter = this->find("X-Forwarded-For");
        if (iter == this->end())
            return this->remote_endpoint_.address();

        auto tokens = util::split(iter->value(), ",");
        if (tokens.empty())
            return this->remote_endpoint_.address();

        boost::system::error_code ec;
        auto address = net::ip::make_address(tokens.front(), ec);
        if (ec)
            return this->remote_endpoint_.address();
        return address;
    }
    const tcp::endpoint& local_endpoint() const { return this->local_endpoint_; }
    const tcp::endpoint& remote_endpoint() const { return this->remote_endpoint_; }

    void set_custom_data(std::any&& data) { this->custom_data_ = std::move(data); }
    std::any& custom_data() { return custom_data_; }
    const std::any& custom_data() const { return custom_data_; }

    void set_session(std::shared_ptr<middleware::session> sess) { session_ = std::move(sess); }
    std::shared_ptr<middleware::session> session() const { return session_; }

    struct buffer_body_read_ctx
    {
        std::shared_ptr<http::request_parser<body::any_body>> parser;
        beast::flat_buffer* buffer = nullptr;
        std::vector<char> buf_data = std::vector<char>(8192);
        http_stream* stream        = nullptr;
        std::chrono::steady_clock::duration read_timeout {30};
        std::string current_chunk;
    };

    void setup_buffer_body_reading(http_stream& stream,
                                   beast::flat_buffer& buffer,
                                   http::request_parser<http::empty_body>&& header_parser,
                                   std::chrono::steady_clock::duration read_timeout)
    {
        auto parser =
            std::make_shared<http::request_parser<body::any_body>>(std::move(header_parser));
        parser->get().body() = body::buffer_body::value_type {};

        buffer_body_ctx_               = std::make_shared<buffer_body_read_ctx>();
        buffer_body_ctx_->parser       = std::move(parser);
        buffer_body_ctx_->buffer       = &buffer;
        buffer_body_ctx_->stream       = &stream;
        buffer_body_ctx_->read_timeout = read_timeout;

        auto& body =
            std::get<body::buffer_body::value_type>(buffer_body_ctx_->parser->get().body());
        body.data = buffer_body_ctx_->buf_data.data();
        body.size = buffer_body_ctx_->buf_data.size();
    }

    void set_is_chunked_handler(bool v) { is_chunked_handler_ = v; }
    bool is_chunked_handler() const { return is_chunked_handler_; }

    void set_is_buffer_body_handler(bool v) { is_buffer_body_handler_ = v; }
    bool is_buffer_body_handler() const { return is_buffer_body_handler_; }

    struct chunk_read_ctx
    {
        std::shared_ptr<http::request_parser<body::any_body>> parser;
        http_stream* stream        = nullptr;
        beast::flat_buffer* buffer = nullptr;
        std::chrono::steady_clock::duration read_timeout {30};

        std::function<std::size_t(std::uint64_t, std::string_view, beast::error_code&)> chunk_cb;
        std::deque<std::string> chunks;
        bool done = false;
    };

    void setup_chunked_reading(http_stream& stream,
                               beast::flat_buffer& buffer,
                               http::request_parser<http::empty_body>&& header_parser,
                               std::chrono::steady_clock::duration read_timeout)
    {
        auto parser =
            std::make_shared<http::request_parser<body::any_body>>(std::move(header_parser));
        chunk_ctx_               = std::make_shared<chunk_read_ctx>();
        chunk_ctx_->parser       = std::move(parser);
        chunk_ctx_->stream       = &stream;
        chunk_ctx_->buffer       = &buffer;
        chunk_ctx_->read_timeout = read_timeout;
        chunk_ctx_->done         = chunk_ctx_->parser->is_done();

        auto ctx             = chunk_ctx_;
        chunk_ctx_->chunk_cb = [ctx](std::uint64_t /*remain*/,
                                     std::string_view body,
                                     beast::error_code&) -> std::size_t {
            ctx->chunks.push_back(std::string(body));
            return body.size();
        };
        chunk_ctx_->parser->on_chunk_body(chunk_ctx_->chunk_cb);
    }

    net::awaitable<std::string_view> read_chunk()
    {
        if (!chunk_ctx_)
            co_return std::string_view {};

        while (chunk_ctx_->chunks.empty() && !chunk_ctx_->done) {
            boost::system::error_code ec;
            chunk_ctx_->stream->expires_after(chunk_ctx_->read_timeout);
            co_await http::async_read_some(*chunk_ctx_->stream,
                                           *chunk_ctx_->buffer,
                                           *chunk_ctx_->parser,
                                           util::net_awaitable[ec]);
            chunk_ctx_->stream->expires_never();
            if (ec)
                throw boost::system::system_error(ec);
            chunk_ctx_->done = chunk_ctx_->parser->is_done();
        }

        if (!chunk_ctx_->chunks.empty()) {
            chunk_buf_ = std::move(chunk_ctx_->chunks.front());
            chunk_ctx_->chunks.pop_front();
            co_return chunk_buf_;
        }
        co_return std::string_view {};
    }

    net::awaitable<std::string_view> read_buffer_body_some()
    {
        if (!buffer_body_ctx_)
            co_return std::string_view {};

        for (;;) {
            if (buffer_body_ctx_->parser->is_done()) {
                chunk_buf_.clear();
                co_return std::string_view {};
            }

            boost::system::error_code ec;
            buffer_body_ctx_->stream->expires_after(buffer_body_ctx_->read_timeout);
            co_await http::async_read_some(*buffer_body_ctx_->stream,
                                           *buffer_body_ctx_->buffer,
                                           *buffer_body_ctx_->parser,
                                           util::net_awaitable[ec]);
            buffer_body_ctx_->stream->expires_never();
            if (ec)
                throw boost::system::system_error(ec);

            auto& body =
                std::get<body::buffer_body::value_type>(buffer_body_ctx_->parser->get().body());
            auto consumed = buffer_body_ctx_->buf_data.size() - body.size;
            if (consumed > 0) {
                buffer_body_ctx_->current_chunk =
                    std::string(buffer_body_ctx_->buf_data.data(), consumed);
                body.data = buffer_body_ctx_->buf_data.data();
                body.size = buffer_body_ctx_->buf_data.size();
                co_return buffer_body_ctx_->current_chunk;
            }
        }
    }

    std::string_view operator[](http::field name) const { return this->base()[name]; }
    std::string_view operator[](std::string_view name) const { return this->base()[name]; }
    std::string_view at(http::field name) const { return this->base().at(name); }
    std::string_view at(std::string_view name) const { return this->base().at(name); }

    bool has(http::field name) const { return this->base().find(name) != this->base().end(); }
    bool has(std::string_view name) const { return this->base().find(name) != this->base().end(); }
    void set(http::field name, std::string_view value) { this->base().set(name, value); }
    void set(std::string_view name, std::string_view value) { this->base().set(name, value); }
    void erase(http::field name) { this->base().erase(name); }
    void erase(std::string_view name) { this->base().erase(name); }

    std::string_view path_param(const std::string& key) const
    {
        auto it = path_params_.find(key);
        if (it != path_params_.end())
            return it->second;
        return {};
    }
    void set_path_param(const std::string& key, const std::string& val) { path_params_[key] = val; }
    void set_path_param(std::unordered_map<std::string, std::string>&& params)
    {
        path_params_ = std::move(params);
    }

    static request make_request(const tcp::endpoint& local_endpoint,
                                const tcp::endpoint& remote_endpoint,
                                http::request<body::any_body>&& other)
    {
        auto _impl =
            std::make_unique<request::impl>(local_endpoint, remote_endpoint, std::move(other));
        return request(std::move(_impl));
    }
    static request make_request(const tcp::endpoint& local_endpoint,
                                const tcp::endpoint& remote_endpoint,
                                http::request<http::empty_body>&& other)
    {
        auto _impl =
            std::make_unique<request::impl>(local_endpoint, remote_endpoint, std::move(other));
        return request(std::move(_impl));
    }

private:
    std::string decoded_path_;
    html::query_params query_params_;

    tcp::endpoint local_endpoint_;
    tcp::endpoint remote_endpoint_;

    std::unordered_map<std::string, std::string> path_params_;
    std::any custom_data_;
    std::shared_ptr<middleware::session> session_;

    bool is_chunked_handler_     = false;
    bool is_buffer_body_handler_ = false;
    std::shared_ptr<chunk_read_ctx> chunk_ctx_;
    std::shared_ptr<buffer_body_read_ctx> buffer_body_ctx_;
    std::string chunk_buf_;
};
} // namespace httplib::server