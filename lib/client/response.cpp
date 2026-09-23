#include "httplib/client/response.hpp"
#include "ndjson_reader_impl.hpp"
#include "response_impl.h"
#include "sse_reader_impl.hpp"
#include <boost/beast/http/error.hpp>
#include <fstream>
#include <utility>
#include <variant>

namespace httplib::client
{

    response::response() = default;

    response::response(response&&) noexcept = default;
    response& response::operator=(response&&) noexcept = default;
    response::~response() = default;

    response::response(std::shared_ptr<impl> impl) : impl_(std::move(impl)) {}

    http::status
    response::result() const
    {
        return impl_->result();
    }

    unsigned
    response::result_int() const
    {
        return impl_->result_int();
    }

    std::string_view
    response::operator[](http::field name) const
    {
        return impl_->headers()[name];
    }

    std::string_view
    response::operator[](std::string_view name) const
    {
        return impl_->headers()[name];
    }

    http::fields const&
    response::headers() const
    {
        return impl_->headers();
    }

    http::fields&
    response::headers()
    {
        return impl_->headers();
    }

    http::fields const&
    response::base() const
    {
        return headers();
    }

    http::fields&
    response::base()
    {
        return headers();
    }

    std::optional<std::uint64_t>
    response::content_length() const
    {
        return impl_->content_length();
    }

    std::string const&
    response::as_string() const
    {
        return impl_->as_string();
    }

    boost::json::value const&
    response::as_json() const
    {
        return impl_->as_json();
    }

    html::form_data const&
    response::as_form_data() const
    {
        return impl_->as_form_data();
    }

    html::query_params const&
    response::as_query_params() const
    {
        return impl_->as_query_params();
    }

    std::unique_ptr<sse_reader>
    response::create_sse_reader()
    {
        return std::make_unique<sse_reader_impl>(impl_);
    }

    std::unique_ptr<ndjson_reader>
    response::create_ndjson_reader()
    {
        return std::make_unique<ndjson_reader_impl>(impl_);
    }

    net::awaitable<boost::system::result<std::string>>
    response::read_string()
    {
        boost::system::error_code ec;
        co_await impl_->read_body([](http::response<body::any_body>& resp)
                                  { resp.body() = body::string_body::value_type {}; },
                                  ec);
        if (ec)
        {
            co_return ec;
        }
        co_return impl_->take_body<std::string>();
    }

    net::awaitable<boost::system::result<boost::json::value>>
    response::read_json()
    {
        boost::system::error_code ec;
        co_await impl_->read_body([](http::response<body::any_body>& resp)
                                  { resp.body() = body::json_body::value_type {}; },
                                  ec);
        if (ec)
        {
            co_return ec;
        }
        co_return impl_->take_body<boost::json::value>();
    }

    net::awaitable<boost::system::result<html::form_data>>
    response::read_form_data()
    {
        boost::system::error_code ec;
        co_await impl_->read_body([](http::response<body::any_body>& resp)
                                  { resp.body() = body::form_data_body::value_type {}; },
                                  ec);
        if (ec)
        {
            co_return ec;
        }
        co_return impl_->take_body<html::form_data>();
    }

    net::awaitable<boost::system::result<html::query_params>>
    response::read_query_params()
    {
        boost::system::error_code ec;
        co_await impl_->read_body([](http::response<body::any_body>& resp)
                                  { resp.body() = body::query_params_body::value_type {}; },
                                  ec);
        if (ec)
        {
            co_return ec;
        }
        co_return impl_->take_body<html::query_params>();
    }

    net::awaitable<boost::system::error_code>
    response::read_body()
    {
        boost::system::error_code ec;
        co_await impl_->read_body(nullptr, ec);
        co_return ec;
    }

    net::awaitable<boost::system::error_code>
    response::read_to_file(fs::path const& save_path)
    {

        body::file_body::value_type fb;
        fb.open(save_path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!fb.is_open())
        {
            co_return boost::system::errc::make_error_code(boost::system::errc::permission_denied);
        }
        boost::system::error_code ec;
        co_await impl_->read_body([&](http::response<body::any_body>& resp) { resp.body() = std::move(fb); }, ec);
        co_return ec;
    }

    net::awaitable<std::size_t>
    response::read_some_raw(net::mutable_buffer const& buffer, boost::system::error_code& ec)
    {
        if (!impl_)
        {
            ec = boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
            co_return 0;
        }
        co_return co_await impl_->read_some_raw(buffer, ec);
    }
    net::awaitable<std::size_t>
    response::read_some_raw(net::mutable_buffer const& buffer)
    {
        boost::system::error_code ec;
        auto bytes = co_await read_some_raw(buffer, ec);
        if (ec)
        {
            throw boost::system::system_error(ec);
        }
        co_return bytes;
    }

    net::awaitable<std::size_t>
    response::read_some_decompressed(net::mutable_buffer const& buffer, boost::system::error_code& ec)
    {
        if (!impl_)
        {
            ec = boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
            co_return 0;
        }
        co_return co_await impl_->read_some_decompressed(buffer, ec);
    }

    net::awaitable<std::size_t>
    response::read_some_decompressed(net::mutable_buffer const& buffer)
    {
        boost::system::error_code ec;
        auto bytes = co_await read_some_decompressed(buffer, ec);
        if (ec)
        {
            throw boost::system::system_error(ec);
        }
        co_return bytes;
    }

    bool
    response::is_body_done() const
    {
        return impl_ && impl_->is_body_done();
    }

} // namespace httplib::client
