#include "httplib/client/response.hpp"
#include "body/read.hpp"
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
        return impl_->header().result();
    }

    unsigned
    response::result_int() const
    {
        return impl_->header().result_int();
    }

    std::string_view
    response::operator[](http::field name) const
    {
        return impl_->header()[name];
    }

    std::string_view
    response::operator[](std::string_view name) const
    {
        return impl_->header()[name];
    }

    http::fields const&
    response::headers() const
    {
        return impl_->header();
    }

    http::fields&
    response::headers()
    {
        return impl_->header();
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
        return impl_->body_state().as_string();
    }

    boost::json::value const&
    response::as_json() const
    {
        return impl_->body_state().as_json();
    }

    html::form_data const&
    response::as_form_data() const
    {
        return impl_->body_state().as_form_data();
    }

    html::query_params const&
    response::as_query_params() const
    {
        return impl_->body_state().as_query_params();
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
        co_return co_await body::read_as<body::string_body>(*impl_);
    }

    net::awaitable<boost::system::result<boost::json::value>>
    response::read_json()
    {
        co_return co_await body::read_as<body::json_body>(*impl_);
    }

    net::awaitable<boost::system::result<html::form_data>>
    response::read_form_data()
    {
        co_return co_await body::read_as<body::form_data_body>(*impl_);
    }

    net::awaitable<boost::system::result<html::query_params>>
    response::read_query_params()
    {
        co_return co_await body::read_as<body::query_params_body>(*impl_);
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
        auto result = co_await body::read_as<body::file_body>(
            *impl_,
            [&](http::response<body::any_body>& resp) { resp.body() = std::move(fb); });
        if (!result)
        {
            co_return result.error();
        }
        co_return boost::system::error_code {};
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
