#include "httplib/server/request.hpp"
#include "httplib/util/misc.hpp"
#include "request_impl.hpp"

namespace httplib::server
{

    request::request(std::unique_ptr<impl>&& _impl) : impl_(std::move(_impl)) {}

    request::request(request&& other) noexcept { impl_ = std::move(other.impl_); }

    request&
    request::operator=(request&& other) noexcept
    {
        impl_ = std::move(other.impl_);
        return *this;
    }
    request::~request() {}
    http::verb
    request::method() const
    {
        return impl_->header().method();
    }
    std::string_view
    request::method_string() const
    {
        return impl_->header().method_string();
    }
    std::string_view
    request::target() const
    {
        return impl_->header().target();
    }
    httplib::http::fields&
    request::base()
    {
        return impl_->header();
    }

    httplib::http::fields const&
    request::base() const
    {
        return impl_->header();
    }

    std::string_view
    request::operator[](http::field name) const
    {
        return impl_->header()[name];
    }

    std::string_view
    request::operator[](std::string_view name) const
    {
        return impl_->header()[name];
    }

    std::string_view
    request::at(http::field name) const
    {
        return impl_->header().at(name);
    }

    std::string_view
    request::at(std::string_view name) const
    {
        return impl_->header().at(name);
    }

    bool
    request::has(http::field name) const
    {
        return impl_->header().find(name) != impl_->header().end();
    }

    bool
    request::has(std::string_view name) const
    {
        return impl_->header().find(name) != impl_->header().end();
    }

    std::string_view
    request::path() const
    {
        return impl_->path();
    }

    httplib::net::ip::address
    request::get_client_ip() const
    {
        return impl_->get_client_ip();
    }

    httplib::tcp::endpoint const&
    request::local_endpoint() const
    {
        return impl_->local_endpoint();
    }

    httplib::tcp::endpoint const&
    request::remote_endpoint() const
    {
        return impl_->remote_endpoint();
    }

    bool
    request::is_ssl() const
    {
        return impl_->is_ssl();
    }

    std::string_view
    request::path_param_raw(std::string const& key) const
    {
        return impl_->path_param(key);
    }

    request_data&
    request::data()
    {
        return impl_->data();
    }
    request_data const&
    request::data() const
    {
        return impl_->data();
    }

    html::query_params const&
    request::query_params() const
    {
        return impl_->query_params();
    }

    std::string const&
    request::as_string() const
    {
        return impl_->reader().state().as_string();
    }

    boost::json::value const&
    request::as_json() const
    {
        return impl_->reader().state().as_json();
    }

    html::form_data const&
    request::as_form_data() const
    {
        return impl_->reader().state().as_form_data();
    }

    html::query_params const&
    request::as_query_params() const
    {
        return impl_->reader().state().as_query_params();
    }

    bool
    request::is_empty() const
    {
        return impl_->reader().state().is_empty();
    }

    bool
    request::is_string() const
    {
        return impl_->reader().state().type() == body::body_state::kind::string;
    }

    bool
    request::is_json() const
    {
        return impl_->reader().state().type() == body::body_state::kind::json;
    }

    bool
    request::is_form_data() const
    {
        return impl_->reader().state().type() == body::body_state::kind::form_data;
    }

    bool
    request::is_query_params() const
    {
        return impl_->reader().state().type() == body::body_state::kind::query_params;
    }

    net::awaitable<std::string>
    request::read_string()
    {
        auto result = co_await impl_->reader().read_string();
        if (!result)
        {
            throw boost::system::system_error(result.error());
        }
        co_return std::move(*result);
    }

    net::awaitable<boost::json::value>
    request::read_json()
    {
        auto result = co_await impl_->reader().read_json();
        if (!result)
        {
            throw boost::system::system_error(result.error());
        }
        co_return std::move(*result);
    }

    net::awaitable<html::form_data>
    request::read_form_data()
    {
        auto result = co_await impl_->reader().read_form_data();
        if (!result)
        {
            throw boost::system::system_error(result.error());
        }
        co_return std::move(*result);
    }

    net::awaitable<html::query_params>
    request::read_query_params()
    {
        auto result = co_await impl_->reader().read_query_params();
        if (!result)
        {
            throw boost::system::system_error(result.error());
        }
        co_return std::move(*result);
    }

    net::awaitable<boost::system::error_code>
    request::read_body()
    {
        co_return co_await impl_->reader().read_body();
    }

    net::awaitable<std::size_t>
    request::read_some_raw(net::mutable_buffer const& buffer, boost::system::error_code& ec)
    {
        co_return co_await impl_->reader().read_some_raw(buffer, ec);
    }
    httplib::net::awaitable<std::size_t>
    request::read_some_raw(net::mutable_buffer const& buffer)
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
    request::read_some_decompressed(net::mutable_buffer const& buffer, boost::system::error_code& ec)
    {
        co_return co_await impl_->reader().read_some_decompressed(buffer, ec);
    }
    net::awaitable<std::size_t>
    request::read_some_decompressed(net::mutable_buffer const& buffer)
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
    request::is_body_done() const
    {
        return impl_->reader().is_body_done();
    }

} // namespace httplib::server
