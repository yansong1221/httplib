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
        return impl_->method();
    }
    std::string_view
    request::method_string() const
    {
        return impl_->method_string();
    }
    std::string_view
    request::target() const
    {
        return impl_->target();
    }
    httplib::http::fields&
    request::base()
    {
        return impl_->base();
    }

    httplib::http::fields const&
    request::base() const
    {
        return impl_->base();
    }

    std::string_view
    request::operator[](http::field name) const
    {
        return (*impl_)[name];
    }

    std::string_view
    request::operator[](std::string_view name) const
    {
        return (*impl_)[name];
    }

    std::string_view
    request::at(http::field name) const
    {
        return impl_->at(name);
    }

    std::string_view
    request::at(std::string_view name) const
    {
        return impl_->at(name);
    }

    bool
    request::has(http::field name) const
    {
        return impl_->has(name);
    }

    bool
    request::has(std::string_view name) const
    {
        return impl_->has(name);
    }

    void
    request::set(http::field name, std::string_view value)
    {
        impl_->set(name, value);
    }

    void
    request::set(std::string_view name, std::string_view value)
    {
        impl_->set(name, value);
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
        return std::get<std::string>(impl_->body());
    }

    boost::json::value const&
    request::as_json() const
    {
        return std::get<boost::json::value>(impl_->body());
    }

    html::form_data const&
    request::as_form_data() const
    {
        return std::get<html::form_data>(impl_->body());
    }

    html::query_params const&
    request::as_query_params() const
    {
        return std::get<html::query_params>(impl_->body());
    }

    bool
    request::is_empty() const
    {
        return impl_->body().template is_body_type<body::empty_body>();
    }

    bool
    request::is_string() const
    {
        return impl_->body().template is_body_type<body::string_body>();
    }

    bool
    request::is_json() const
    {
        return impl_->body().template is_body_type<body::json_body>();
    }

    bool
    request::is_form_data() const
    {
        return impl_->body().template is_body_type<body::form_data_body>();
    }

    bool
    request::is_query_params() const
    {
        return impl_->body().template is_body_type<body::query_params_body>();
    }

    bool
    request::is_lazy() const
    {
        return impl_->is_lazy();
    }

    net::awaitable<std::string>
    request::read_string()
    {
        auto ec = co_await impl_->read_body([](http::request<body::any_body>& req)
                                            { req.body() = body::string_body::value_type {}; });
        if (ec)
        {
            throw boost::system::system_error(ec);
        }
        co_return std::move(std::get<std::string>(impl_->body()));
    }

    net::awaitable<boost::json::value>
    request::read_json()
    {
        auto ec = co_await impl_->read_body([](http::request<body::any_body>& req)
                                            { req.body() = body::json_body::value_type {}; });
        if (ec)
        {
            throw boost::system::system_error(ec);
        }
        co_return std::move(std::get<boost::json::value>(impl_->body()));
    }

    net::awaitable<html::form_data>
    request::read_form_data()
    {
        auto ec = co_await impl_->read_body([](http::request<body::any_body>& req)
                                            { req.body() = body::form_data_body::value_type {}; });
        if (ec)
        {
            throw boost::system::system_error(ec);
        }
        co_return std::move(std::get<html::form_data>(impl_->body()));
    }

    net::awaitable<html::query_params>
    request::read_query_params()
    {
        auto ec = co_await impl_->read_body([](http::request<body::any_body>& req)
                                            { req.body() = body::query_params_body::value_type {}; });
        if (ec)
        {
            throw boost::system::system_error(ec);
        }
        co_return std::move(std::get<html::query_params>(impl_->body()));
    }

    net::awaitable<void>
    request::read_body()
    {
        auto ec = co_await impl_->read_body(nullptr);
        if (ec)
        {
            throw boost::system::system_error(ec);
        }
        co_return;
    }

    net::awaitable<std::size_t>
    request::read_some_raw(net::mutable_buffer const& buffer)
    {
        auto result = co_await impl_->read_some_raw(buffer);
        if (result.has_error())
        {
            throw boost::system::system_error(result.error());
        }
        co_return result.value();
    }

    net::awaitable<std::size_t>
    request::read_some_decompressed(net::mutable_buffer const& buffer)
    {
        auto result = co_await impl_->read_some_decompressed(buffer);
        if (result.has_error())
        {
            throw boost::system::system_error(result.error());
        }
        co_return result.value();
    }

    bool
    request::is_body_done() const
    {
        return impl_->is_body_done();
    }

} // namespace httplib::server
