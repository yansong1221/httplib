#include "httplib/client/response.hpp"
#include "response_impl.h"

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

    body::any_body::value_type&
    response::body()
    {
        return impl_->msg_.body();
    }

    body::any_body::value_type const&
    response::body() const
    {
        return impl_->msg_.body();
    }

    unsigned
    response::version() const
    {
        return impl_->msg_.version();
    }

    bool
    response::keep_alive() const
    {
        return impl_->msg_.keep_alive();
    }

    std::string_view
    response::reason() const
    {
        return impl_->msg_.reason();
    }

} // namespace httplib::client
