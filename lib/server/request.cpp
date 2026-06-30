#include "httplib/server/middleware/session.hpp"
#include "httplib/server/request.hpp"
#include "httplib/util/misc.hpp"
#include "request_impl.hpp"

namespace httplib::server {

request::request(std::unique_ptr<impl>&& _impl)
    : impl_(std::move(_impl))
{
}

request::request(request&& other) noexcept
{
    impl_ = std::move(other.impl_);
}

request& request::operator=(request&& other) noexcept
{
    impl_ = std::move(other.impl_);
    return *this;
}
request::~request()
{
}
http::verb request::method() const
{
    return impl_->method();
}
std::string_view request::method_string() const
{
    return impl_->method_string();
}
std::string_view request::target() const
{
    return impl_->target();
}
httplib::http::fields& request::base()
{
    return impl_->base();
}

const httplib::http::fields& request::base() const
{
    return impl_->base();
}

std::string_view request::operator[](http::field name) const
{
    return (*impl_)[name];
}

std::string_view request::operator[](std::string_view name) const
{
    return (*impl_)[name];
}

header_proxy request::operator[](http::field name)
{
    return header_proxy(base(), name);
}

header_proxy request::operator[](std::string_view name)
{
    return header_proxy(base(), name);
}

std::string_view request::at(http::field name) const
{
    return impl_->at(name);
}

std::string_view request::at(std::string_view name) const
{
    return impl_->at(name);
}

bool request::has(http::field name) const
{
    return impl_->has(name);
}

bool request::has(std::string_view name) const
{
    return impl_->has(name);
}

void request::set(http::field name, std::string_view value)
{
    impl_->set(name, value);
}

void request::set(std::string_view name, std::string_view value)
{
    impl_->set(name, value);
}

void request::erase(http::field name)
{
    impl_->erase(name);
}

void request::erase(std::string_view name)
{
    impl_->erase(name);
}

std::string_view request::path() const
{
    return impl_->path();
}

httplib::net::ip::address request::get_client_ip() const
{
    return impl_->get_client_ip();
}

const httplib::tcp::endpoint& request::local_endpoint() const
{
    return impl_->local_endpoint();
}

const httplib::tcp::endpoint& request::remote_endpoint() const
{
    return impl_->remote_endpoint();
}

void request::set_custom_data(std::any&& data)
{
    impl_->set_custom_data(std::move(data));
}

std::string_view request::path_param(const std::string& key) const
{
    return impl_->path_param(key);
}

void request::set_path_param(const std::string& key, const std::string& val)
{
    impl_->set_path_param(key, val);
}
void request::set_path_param(std::unordered_map<std::string, std::string>&& params)
{
    impl_->set_path_param(std::move(params));
}

const html::query_params& request::query_params() const
{
    return impl_->query_params();
}

std::any& request::any_custom_data()
{
    return impl_->custom_data();
}

const std::any& request::any_custom_data() const
{
    return impl_->custom_data();
}

void request::set_session(std::shared_ptr<middleware::session> sess)
{
    impl_->set_session(std::move(sess));
}

std::shared_ptr<middleware::session> request::session() const
{
    return impl_->session();
}

request::impl* request::get_impl()
{
    return impl_.get();
}

const request::impl* request::get_impl() const
{
    return impl_.get();
}

httplib::body::any_body::value_type& request::body()
{
    return impl_->body();
}

const httplib::body::any_body::value_type& request::body() const
{
    return impl_->body();
}

} // namespace httplib::server