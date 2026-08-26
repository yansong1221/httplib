#include "httplib/client/response.hpp"
#include "response_impl.h"
#include <variant>

namespace httplib::client
{

    response::response() : impl_(std::make_shared<impl>()) {}

    response::response(response&&) noexcept = default;
    response&
    response::operator=(response&&) noexcept = default;
    response::~response() = default;

    response::response(std::shared_ptr<impl> impl) : impl_(std::move(impl)) {}

    http::status
    response::result() const
    {
        return impl_->msg_.result();
    }

    unsigned
    response::result_int() const
    {
        return impl_->msg_.result_int();
    }

    std::string_view
    response::operator[](http::field name) const
    {
        return impl_->msg_[name];
    }

    std::string_view
    response::operator[](std::string_view name) const
    {
        return impl_->msg_[name];
    }

    http::fields const&
    response::headers() const
    {
        return impl_->msg_.base();
    }

    http::fields&
    response::headers()
    {
        return impl_->msg_.base();
    }

    http::fields const&
    response::base() const
    {
        return impl_->msg_.base();
    }

    http::fields&
    response::base()
    {
        return impl_->msg_.base();
    }

    std::string const&
    response::as_string() const
    {
        return std::get<std::string>(impl_->msg_.body());
    }

    boost::json::value const&
    response::as_json() const
    {
        return std::get<boost::json::value>(impl_->msg_.body());
    }

    html::form_data const&
    response::as_form_data() const
    {
        return std::get<html::form_data>(impl_->msg_.body());
    }

    html::query_params const&
    response::as_query_params() const
    {
        return std::get<html::query_params>(impl_->msg_.body());
    }

} // namespace httplib::client
