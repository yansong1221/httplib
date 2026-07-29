#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "stream/http_stream.hpp"
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>

namespace httplib::detail {

template<bool isRequest>
class chunk_reader_core
{
public:
    using parser_type = http::parser<isRequest, body::any_body>;

    chunk_reader_core() = default;

    void setup(http_stream& stream,
               beast::flat_buffer& buffer,
               parser_type& parser,
               std::chrono::steady_clock::duration read_timeout)
    {
        ctx_               = std::make_shared<ctx>();
        ctx_->stream       = &stream;
        ctx_->buffer       = &buffer;
        ctx_->parser       = &parser;
        ctx_->read_timeout = read_timeout;
        ctx_->done         = parser.is_done();

        auto ctx             = ctx_;
        ctx_->chunk_cb       = [ctx](std::uint64_t /*remain*/,
                               std::string_view body,
                               beast::error_code&) -> std::size_t {
            ctx->chunks.push_back(std::string(body));
            return body.size();
        };
        parser.on_chunk_body(ctx_->chunk_cb);
    }

    net::awaitable<std::string_view> read_chunk()
    {
        if (!ctx_)
            co_return std::string_view {};

        while (ctx_->chunks.empty() && !ctx_->done) {
            boost::system::error_code ec;
            ctx_->stream->expires_after(ctx_->read_timeout);
            co_await http::async_read_some(*ctx_->stream,
                                           *ctx_->buffer,
                                           *ctx_->parser,
                                           util::net_awaitable[ec]);
            ctx_->stream->expires_never();
            if (ec)
                throw boost::system::system_error(ec);
            ctx_->done = ctx_->parser->is_done();
        }

        if (!ctx_->chunks.empty()) {
            current_chunk_ = std::move(ctx_->chunks.front());
            ctx_->chunks.pop_front();
            co_return current_chunk_;
        }
        co_return std::string_view {};
    }

    bool is_done() const { return ctx_ && ctx_->done && ctx_->chunks.empty(); }

private:
    struct ctx
    {
        parser_type* parser        = nullptr;
        http_stream* stream        = nullptr;
        beast::flat_buffer* buffer = nullptr;
        std::chrono::steady_clock::duration read_timeout {30};

        std::function<std::size_t(std::uint64_t, std::string_view, beast::error_code&)> chunk_cb;
        std::deque<std::string> chunks;
        bool done = false;
    };

    std::shared_ptr<ctx> ctx_;
    std::string current_chunk_;
};

} // namespace httplib::detail
