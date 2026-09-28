#include "httplib/client/response.hpp"
#include "headers_impl.hpp"
#include "ndjson_reader_impl.hpp"
#include "response_impl.h"
#include "sse_reader_impl.hpp"
#include <boost/beast/http/error.hpp>
#include <fstream>
#include <utility>
#include <variant>
#include "beast_alias.hpp"
#include "enum_conv.hpp"

namespace httplib::client
{

    response::response() = default;

    response::response(response&&) noexcept = default;
    response& response::operator=(response&&) noexcept = default;
    response::~response() = default;

    response::response(std::shared_ptr<impl> impl) : impl_(std::move(impl)) {}

    httplib::status
    response::result() const
    {
        return enum_conv::to_status(impl_->get().result());
    }

    unsigned
    response::result_int() const
    {
        return impl_->get().result_int();
    }

    std::string_view
    response::operator[](httplib::field name) const
    {
        return impl_->get()[enum_conv::to_field(name)];
    }

    std::string_view
    response::operator[](std::string_view name) const
    {
        return impl_->get()[name];
    }

    std::string_view
    response::at(httplib::field name) const
    {
        return impl_->get().at(enum_conv::to_field(name));
    }

    std::string_view
    response::at(std::string_view name) const
    {
        return impl_->get().at(name);
    }

    bool
    response::has(httplib::field name) const
    {
        return impl_->get().find(enum_conv::to_field(name)) != impl_->get().end();
    }

    bool
    response::has(std::string_view name) const
    {
        return impl_->get().find(name) != impl_->get().end();
    }

    std::size_t
    response::count(httplib::field name) const
    {
        return impl_->get().count(enum_conv::to_field(name));
    }

    std::size_t
    response::count(std::string_view name) const
    {
        return impl_->get().count(name);
    }

    httplib::headers
    response::headers() const
    {
        // shared_ptr 的 const 不传递到被指对象，impl_ 可变，视图直接借用消息的字段集合。
        return httplib::detail::headers_access::borrow(impl_->get());
    }

    httplib::headers
    response::headers()
    {
        return httplib::detail::headers_access::borrow(impl_->get());
    }

    httplib::headers
    response::base() const
    {
        return headers();
    }

    httplib::headers
    response::base()
    {
        return headers();
    }

    std::optional<std::uint64_t>
    response::content_length() const
    {
        auto len = impl_->content_length();
        if (!len)
        {
            return std::nullopt;
        }
        return len.value();
    }

    std::string const&
    response::as_string() const
    {
        return impl_->state().as<std::string>();
    }

    boost::json::value const&
    response::as_json() const
    {
        return impl_->state().as<boost::json::value>();
    }

    httplib::form_data const&
    response::as_form_data() const
    {
        return impl_->state().as<httplib::form_data>();
    }

    httplib::query_params const&
    response::as_query_params() const
    {
        return impl_->state().as<httplib::query_params>();
    }

    body_type
    response::type() const
    {
        return impl_->state().type();
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
        co_return co_await impl_->read_string();
    }

    net::awaitable<boost::system::result<boost::json::value>>
    response::read_json()
    {
        co_return co_await impl_->read_json();
    }

    net::awaitable<boost::system::result<httplib::form_data>>
    response::read_form_data()
    {
        co_return co_await impl_->read_form_data();
    }

    net::awaitable<boost::system::result<httplib::query_params>>
    response::read_query_params()
    {
        co_return co_await impl_->read_query_params();
    }

    net::awaitable<boost::system::error_code>
    response::read_body()
    {
        co_return co_await impl_->read_body();
    }

    net::awaitable<boost::system::error_code>
    response::read_to_file(fs::path const& save_path)
    {
        co_return co_await impl_->read_to_file(save_path);
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
