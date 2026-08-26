#include "httplib/client/lazy_response.hpp"
#include "compress/compressor.hpp"
#include "lazy_response_impl.h"
#include "ndjson_reader_impl.hpp"
#include "sse_reader_impl.hpp"
#include <array>
#include <boost/beast/http/error.hpp>
#include <fstream>

namespace httplib::client
{

    lazy_response::lazy_response(std::shared_ptr<impl> impl) : impl_(std::move(impl)) {}

    lazy_response::~lazy_response() = default;

    http::status
    lazy_response::result() const
    {
        return impl_->result();
    }

    unsigned
    lazy_response::result_int() const
    {
        return static_cast<unsigned>(result());
    }

    std::string_view
    lazy_response::operator[](http::field name) const
    {
        return impl_->headers()[name];
    }

    std::string_view
    lazy_response::operator[](std::string_view name) const
    {
        return impl_->headers()[name];
    }

    http::fields const&
    lazy_response::headers() const
    {
        return impl_->headers();
    }

    http::fields&
    lazy_response::headers()
    {
        return impl_->headers();
    }

    http::fields const&
    lazy_response::base() const
    {
        return headers();
    }

    http::fields&
    lazy_response::base()
    {
        return headers();
    }

    std::unique_ptr<sse_reader>
    lazy_response::create_sse_reader()
    {
        return std::make_unique<sse_reader_impl>(impl_);
    }

    std::unique_ptr<ndjson_reader>
    lazy_response::create_ndjson_reader()
    {
        return std::make_unique<ndjson_reader_impl>(impl_);
    }

    namespace detail
    {
        template <typename Consume>
        static net::awaitable<boost::system::error_code>
        drain(std::shared_ptr<lazy_response::impl> const& impl, http::fields const& header, Consume&& consume)
        {
            auto encoding = header[http::field::content_encoding];
            auto compressor = compress::compressor_factory::instance().create(encoding);
            if (compressor)
            {
                compressor->init(compress::compressor::mode::decode);
            }

            boost::system::error_code ec;
            std::array<char, 8192> buf;
            for (;;)
            {
                auto result = co_await impl->read_some(net::buffer(buf));
                if (result.has_error())
                {
                    ec = result.error();
                    break;
                }
                auto n = result.value();
                if (n == 0)
                {
                    break;
                }
                if (compressor)
                {
                    compressor->write(net::buffer(buf.data(), n), true);
                    auto decoded = compressor->buffer();
                    if (decoded.size() != 0)
                    {
                        consume(net::const_buffer(decoded.data(), decoded.size()));
                        compressor->consume_all();
                    }
                }
                else
                {
                    consume(net::const_buffer(buf.data(), n));
                }
            }
            if (compressor)
            {
                compressor->finish();
                auto decoded = compressor->buffer();
                if (decoded.size() != 0)
                {
                    consume(net::const_buffer(decoded.data(), decoded.size()));
                    compressor->consume_all();
                }
            }
            co_return ec;
        }

    } // namespace detail

    net::awaitable<body::any_body::value_type>
    lazy_response::read_body()
    {
        auto result = co_await impl_->read_body(nullptr);
        if (result.has_error())
        {
            co_return body::any_body::value_type {};
        }
        co_return std::move(result->body());
    }

    net::awaitable<std::string>
    lazy_response::read_text()
    {
        auto result = co_await impl_->read_body([](http::response<body::any_body>& resp)
                                                { resp.body() = body::string_body::value_type {}; });
        if (result.has_error())
        {
            co_return std::string {};
        }
        co_return result->body().as<body::string_body>();
    }

    net::awaitable<boost::json::value>
    lazy_response::read_json()
    {
        auto result = co_await impl_->read_body([](http::response<body::any_body>& resp)
                                                { resp.body() = body::json_body::value_type {}; });
        if (result.has_error())
        {
            co_return boost::json::value {};
        }
        co_return result->body().as<body::json_body>();
    }

    net::awaitable<boost::system::error_code>
    lazy_response::read_to_file(fs::path const& save_path)
    {
        body::file_body::value_type fb;
        fb.open(save_path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!fb.is_open())
        {
            co_return boost::system::errc::make_error_code(boost::system::errc::permission_denied);
        }

        auto result = co_await impl_->read_body(
            [&](http::response<body::any_body>& resp) { resp.body() = std::move(fb); });
        if (result.has_error())
        {
            co_return result.error();
        }
        co_return boost::system::error_code {};
    }

    net::awaitable<boost::system::result<std::size_t>>
    lazy_response::read_some(net::mutable_buffer const& buffer)
    {
        if (!impl_)
        {
            co_return boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
        }
        co_return co_await impl_->read_some(buffer);
    }

    bool
    lazy_response::is_body_done() const
    {
        return impl_ && impl_->is_body_done();
    }

} // namespace httplib::client
