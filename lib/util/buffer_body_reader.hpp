#pragma once
#include "httplib/util/use_awaitable.hpp"
#include "stream/http_stream.hpp"
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <chrono>
#include <memory>
#include <vector>

namespace httplib::detail
{

    template <bool isRequest>
    class buffer_body_reader
    {
      public:
        void
        setup(http_stream& stream,
              beast::flat_buffer& buffer,
              http::parser<isRequest, http::buffer_body>& parser,
              std::chrono::steady_clock::duration read_timeout)
        {
            stream_ = &stream;
            buffer_ = &buffer;
            parser_ = &parser;
            read_timeout_ = read_timeout;
        }

        net::awaitable<std::size_t>
        read_some(net::mutable_buffer const& buffer)
        {
            if (parser_->is_done())
            {
                co_return 0;
            }

            for (;;)
            {
                auto& b = parser_->get().body();
                b.data = buffer.data();
                b.size = buffer.size();

                boost::system::error_code ec;
                stream_->expires_after(read_timeout_);
                co_await http::async_read_some(*stream_, *buffer_, *parser_, util::net_awaitable[ec]);
                stream_->expires_never();
                if (ec)
                {
                    throw boost::system::system_error(ec);
                }

                auto& body = parser_->get().body();
                auto consumed = buffer.size() - body.size;
                if (consumed > 0)
                {
                    co_return consumed;
                }
            }
        }

        bool
        is_done() const
        {
            return parser_ && parser_->is_done();
        }

      private:
        http::parser<isRequest, http::buffer_body>* parser_ = nullptr;
        http_stream* stream_ = nullptr;
        beast::flat_buffer* buffer_ = nullptr;
        std::chrono::steady_clock::duration read_timeout_ { 30 };
    };

} // namespace httplib::detail
