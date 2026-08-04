#include "httplib/server/request.hpp"
#include "httplib/server/middleware/session.hpp"
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

    void
    request::erase(http::field name)
    {
        impl_->erase(name);
    }

    void
    request::erase(std::string_view name)
    {
        impl_->erase(name);
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
    request::path_param(std::string const& key) const
    {
        return impl_->path_param(key);
    }

    void
    request::set_path_param(std::string const& key, std::string const& val)
    {
        impl_->set_path_param(key, val);
    }
    void
    request::set_path_param(std::unordered_map<std::string, std::string>&& params)
    {
        impl_->set_path_param(std::move(params));
    }

    html::query_params const&
    request::query_params() const
    {
        return impl_->query_params();
    }

    void
    request::set_custom_data(std::string key, std::any value)
    {
        impl_->set_custom_data(std::move(key), std::move(value));
    }

    std::any&
    request::custom_data(std::string const& key)
    {
        return impl_->custom_data(key);
    }

    std::any const&
    request::custom_data(std::string const& key) const
    {
        return impl_->custom_data(key);
    }

    bool
    request::has_custom_data(std::string const& key) const
    {
        return impl_->has_custom_data(key);
    }

    void
    request::set_session(std::shared_ptr<middleware::session> sess)
    {
        impl_->set_session(std::move(sess));
    }

    std::shared_ptr<middleware::session>
    request::session() const
    {
        return impl_->session();
    }

    request::impl*
    request::get_impl()
    {
        return impl_.get();
    }

    request::impl const*
    request::get_impl() const
    {
        return impl_.get();
    }

    httplib::body::any_body::value_type&
    request::body()
    {
        return impl_->body();
    }

    httplib::body::any_body::value_type const&
    request::body() const
    {
        return impl_->body();
    }

    bool
    request::is_chunked() const
    {
        return impl_->is_chunked();
    }

    chunk_reader*
    request::get_chunk_reader()
    {
        return impl_->get_chunk_reader();
    }

} // namespace httplib::server