#pragma once
#include "body/codec.hpp"
#include "compress/compressor.hpp"
#include "httplib/server/stream_writer.hpp"
#include "response_impl.hpp"
#include <boost/beast/http/field.hpp>
#include <boost/system/error_code.hpp>
#include <memory>
#include <string>

namespace httplib::server
{

    class stream_writer_impl : public stream_writer
    {
      public:
        explicit stream_writer_impl(response::impl& resp) : resp_(resp) {}

        net::awaitable<void>
        write_header(http::status status, http::fields const& headers, mode m) override
        {
            boost::system::error_code ec;
            co_await write_header(status, headers, m, ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }
        net::awaitable<void>
        write_header(http::status status, http::fields const& headers, mode m, boost::system::error_code& ec) override
        {
            for (auto const& f : headers)
            {
                resp_.base().erase(f.name_string());
            }
            resp_.base().result(status);
            for (auto const& f : headers)
            {
                resp_.base().insert(f.name_string(), f.value());
            }

            encoder_.reset();
            if (m == mode::chunked)
            {
                auto encoding = resp_.base()[http::field::content_encoding];
                if (!encoding.empty() && compress::compressor_factory::instance().is_transform_encoding(encoding))
                {
                    encoder_ = std::make_unique<body::stream_encoder>();
                    encoder_->reset(encoding, ec);
                    if (ec)
                    {
                        resp_.keep_alive(false);
                        co_return;
                    }
                }
            }

            auto writer_mode = m == mode::relay ? response::impl::body_writer_t::stream_mode::relay
                                                : response::impl::body_writer_t::stream_mode::chunked;
            ec = co_await resp_.writer().begin_stream(writer_mode);
        }

        net::awaitable<void>
        write_body(net::const_buffer const& data, bool more) override
        {
            boost::system::error_code ec;
            co_await write_body(data, more, ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }
        net::awaitable<void>
        write_body(net::const_buffer const& data, bool more, boost::system::error_code& ec) override
        {
            if (!encoder_)
            {
                ec = co_await resp_.writer().write_some(data, more);
                co_return;
            }

            encoder_->consume_all();
            encoder_->feed(data, more, ec);
            if (ec)
            {
                resp_.keep_alive(false);
                co_return;
            }

            auto buffer = encoder_->buffer();
            if (buffer.size() == 0 && more)
            {
                co_return;
            }
            ec = co_await resp_.writer().write_some(buffer, more);
        }

      private:
        response::impl& resp_;
        std::unique_ptr<body::stream_encoder> encoder_;
    };

} // namespace httplib::server
