#include "httplib/client/request.hpp"
#include "body/body_state.hpp"
#include "compress/compressor.hpp"
#include "request_impl.h"
#include <boost/algorithm/string/join.hpp>
#include <boost/beast/version.hpp>
#include <boost/json/value.hpp>
#include <fmt/format.h>
#include <fstream>

namespace httplib::client
{
    namespace detail
    {

        static std::string
        make_target(std::string_view path, html::query_params const& params)
        {
            std::string target(path);
            if (!params.empty())
            {
                target += target.find('?') == std::string::npos ? "?" : "&";
                target += params.encoded();
            }
            return target;
        }

    } // namespace detail

    request::request(http::verb method, std::string_view target, http::fields const& headers)
        : impl_(std::make_shared<impl>(method, target, 11))
    {
        impl_->set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        impl_->set(http::field::accept, "*/*");

        auto const& encoding = compress::compressor_factory::instance().supported_encoding();
        if (!encoding.empty())
        {
            impl_->set(http::field::accept_encoding, boost::join(encoding, ","));
        }

        for (auto const& field : headers)
        {
            impl_->set(field.name_string(), field.value());
        }
        impl_->keep_alive(true);
    }

    request::request(request&&) noexcept = default;
    request& request::operator=(request&&) noexcept = default;
    request::~request() = default;

    request::request(std::shared_ptr<impl> impl) : impl_(std::move(impl)) {}

    request::request(http::verb method,
                     std::string_view path,
                     html::query_params const& params,
                     http::fields const& headers /*= http::fields()*/)
        : request(method, detail::make_target(path, params), headers)
    {
    }

    http::verb
    request::method() const
    {
        return impl_->method();
    }

    void
    request::method(http::verb v)
    {
        impl_->method(v);
    }

    std::string_view
    request::target() const
    {
        return impl_->target();
    }

    void
    request::target(std::string_view t)
    {
        impl_->target(t);
    }

    std::string_view
    request::operator[](http::field name) const
    {
        return impl_->base()[name];
    }

    std::string_view
    request::operator[](std::string_view name) const
    {
        return impl_->base()[name];
    }

    std::string_view
    request::at(http::field name) const
    {
        return impl_->base().at(name);
    }

    std::string_view
    request::at(std::string_view name) const
    {
        return impl_->base().at(name);
    }
    void
    request::insert(http::field name, std::string_view value)
    {
        impl_->insert(name, value);
    }

    void
    request::insert(std::string_view name, std::string_view value)
    {
        impl_->insert(name, value);
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
        impl_->base().erase(name);
    }

    bool
    request::has(http::field name) const
    {
        return impl_->base().find(name) != impl_->base().end();
    }

    bool
    request::has(std::string_view name) const
    {
        return impl_->base().find(name) != impl_->base().end();
    }

    http::fields&
    request::base()
    {
        return impl_->base();
    }

    http::fields const&
    request::base() const
    {
        return impl_->base();
    }

    std::string const&
    request::as_string() const
    {
        return body::access::as_string(impl_->body());
    }

    boost::json::value const&
    request::as_json() const
    {
        return body::access::as_json(impl_->body());
    }

    html::form_data const&
    request::as_form_data() const
    {
        return body::access::as_form_data(impl_->body());
    }

    html::query_params const&
    request::as_query_params() const
    {
        return body::access::as_query_params(impl_->body());
    }

    bool
    request::is_empty() const
    {
        return body::access::is<body::empty_body>(impl_->body());
    }

    bool
    request::is_string() const
    {
        return body::access::is<body::string_body>(impl_->body());
    }

    bool
    request::is_json() const
    {
        return body::access::is<body::json_body>(impl_->body());
    }

    bool
    request::is_form_data() const
    {
        return body::access::is<body::form_data_body>(impl_->body());
    }

    bool
    request::is_query_params() const
    {
        return body::access::is<body::query_params_body>(impl_->body());
    }

    bool
    request::is_file() const
    {
        return body::access::is<body::file_body>(impl_->body());
    }

    void
    request::content_length(std::uint64_t n)
    {
        impl_->content_length(n);
    }

    bool
    request::keep_alive() const
    {
        return impl_->keep_alive();
    }

    void
    request::keep_alive(bool value)
    {
        impl_->keep_alive(value);
    }

    void
    request::set_body(std::string_view data, std::string_view content_type)
    {
        set_body(std::string(data), content_type);
    }

    void
    request::set_body(std::string&& data, std::string_view content_type)
    {
        impl_->set(http::field::content_type, content_type);
        impl_->content_length(data.size());
        impl_->body() = std::move(data);
    }

    void
    request::set_body(boost::json::value&& data)
    {
        impl_->set(http::field::content_type, "application/json");
        impl_->body() = std::move(data);
        impl_->prepare_payload();
    }

    void
    request::set_body(html::form_data&& data)
    {
        impl_->set(http::field::content_type, fmt::format("multipart/form-data; boundary={}", data.boundary));
        impl_->body() = std::move(data);
        impl_->prepare_payload();
    }

    void
    request::set_body(html::query_params&& data)
    {
        impl_->set(http::field::content_type, "application/x-www-form-urlencoded");
        impl_->body() = std::move(data);
        impl_->prepare_payload();
    }

    void
    request::set_file_body(fs::path const& path)
    {
        body::file_body::value_type file_body;
        file_body.open(path, std::ios::in | std::ios::binary);
        impl_->body() = std::move(file_body);
        impl_->prepare_payload();
    }

} // namespace httplib::client
