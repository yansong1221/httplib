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

    response::response() : impl_(std::make_shared<impl>()) {}

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
        auto ec = co_await impl_->read_body([](http::response<body::any_body>& resp)
                                            { resp.body() = body::string_body::value_type {}; });
        if (ec)
        {
            co_return ec;
        }
        co_return as_string();
    }

    net::awaitable<boost::system::result<boost::json::value>>
    response::read_json()
    {
        auto ec = co_await impl_->read_body([](http::response<body::any_body>& resp)
                                            { resp.body() = body::json_body::value_type {}; });
        if (ec)
        {
            co_return ec;
        }
        co_return as_json();
    }

    net::awaitable<boost::system::result<html::form_data>>
    response::read_form_data()
    {
        auto ec = co_await impl_->read_body([](http::response<body::any_body>& resp)
                                            { resp.body() = body::form_data_body::value_type {}; });
        if (ec)
        {
            co_return ec;
        }
        co_return as_form_data();
    }

    net::awaitable<boost::system::result<html::query_params>>
    response::read_query_params()
    {
        auto ec = co_await impl_->read_body([](http::response<body::any_body>& resp)
                                            { resp.body() = body::query_params_body::value_type {}; });
        if (ec)
        {
            co_return ec;
        }
        co_return as_query_params();
    }

    net::awaitable<boost::system::result<response>>
    response::read_body()
    {
        auto ec = co_await impl_->read_body(nullptr);
        if (ec)
        {
            co_return ec;
        }
        co_return std::move(*this);
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
        co_return co_await impl_->read_body([&](http::response<body::any_body>& resp) { resp.body() = std::move(fb); });
    }

    net::awaitable<boost::system::result<std::size_t>>
    response::read_some_raw(net::mutable_buffer const& buffer)
    {
        if (!impl_)
        {
            co_return boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
        }
        co_return co_await impl_->read_some_raw(buffer);
    }

    net::awaitable<boost::system::result<std::size_t>>
    response::read_some_decompressed(net::mutable_buffer const& buffer)
    {
        if (!impl_)
        {
            co_return boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
        }
        co_return co_await impl_->read_some_decompressed(buffer);
    }

    bool
    response::is_body_done() const
    {
        return impl_ && impl_->is_body_done();
    }

} // namespace httplib::client
