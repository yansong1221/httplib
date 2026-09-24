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
        return impl_->reader().state().as_string();
    }

    boost::json::value const&
    response::as_json() const
    {
        return impl_->reader().state().as_json();
    }

    html::form_data const&
    response::as_form_data() const
    {
        return impl_->reader().state().as_form_data();
    }

    html::query_params const&
    response::as_query_params() const
    {
        return impl_->reader().state().as_query_params();
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
        co_return co_await impl_->reader().read_string();
    }

    net::awaitable<boost::system::result<boost::json::value>>
    response::read_json()
    {
        co_return co_await impl_->reader().read_json();
    }

    net::awaitable<boost::system::result<html::form_data>>
    response::read_form_data()
    {
        co_return co_await impl_->reader().read_form_data();
    }

    net::awaitable<boost::system::result<html::query_params>>
    response::read_query_params()
    {
        co_return co_await impl_->reader().read_query_params();
    }

    net::awaitable<boost::system::error_code>
    response::read_body()
    {
        co_return co_await impl_->reader().read_body();
    }

    net::awaitable<boost::system::error_code>
    response::read_to_file(fs::path const& save_path)
    {
        co_return co_await impl_->reader().read_to_file(save_path);
    }

    net::awaitable<std::size_t>
    response::read_some_raw(net::mutable_buffer const& buffer, boost::system::error_code& ec)
    {
        if (!impl_)
        {
            ec = boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
            co_return 0;
        }
        co_return co_await impl_->reader().read_some_raw(buffer, ec);
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
        co_return co_await impl_->reader().read_some_decompressed(buffer, ec);
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
        return impl_ && impl_->reader().is_body_done();
    }

} // namespace httplib::client
