#include "httplib/client/request.hpp"
#include "compress/compressor.hpp"
#include "headers_impl.hpp"
#include "request_impl.h"
#include "util/mime_types.hpp"
#include <boost/algorithm/string/join.hpp>
#include <boost/beast/version.hpp>
#include <boost/json/value.hpp>
#include <fmt/format.h>
#include <fstream>
#include "beast_alias.hpp"
#include "enum_conv.hpp"

namespace httplib::client
{
    namespace detail
    {

        static std::string
        make_target(std::string_view path, httplib::query_params const& params)
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

    request::request(httplib::method method, std::string_view target, httplib::headers const& headers)
        : impl_(std::make_shared<impl>(enum_conv::to_verb(method), target))
    {
        replace(headers);
    }

    request::request(request&&) noexcept = default;
    request& request::operator=(request&&) noexcept = default;
    request::~request() = default;

    request::request(std::shared_ptr<impl> impl) : impl_(std::move(impl)) {}

    request::request(httplib::method method,
                     std::string_view path,
                     httplib::query_params const& params,
                     httplib::headers const& headers)
        : request(method, detail::make_target(path, params))
    {
        replace(headers);
    }

    httplib::method
    request::method() const
    {
        return enum_conv::to_method(impl_->base().method());
    }

    void
    request::method(httplib::method v)
    {
        impl_->base().method(enum_conv::to_verb(v));
    }

    std::string_view
    request::target() const
    {
        return impl_->base().target();
    }

    void
    request::target(std::string_view t)
    {
        impl_->base().target(t);
    }

    std::string_view
    request::operator[](httplib::field name) const
    {
        return impl_->base()[enum_conv::to_field(name)];
    }

    std::string_view
    request::operator[](std::string_view name) const
    {
        return impl_->base()[name];
    }

    std::string_view
    request::at(httplib::field name) const
    {
        return impl_->base().at(enum_conv::to_field(name));
    }

    std::string_view
    request::at(std::string_view name) const
    {
        return impl_->base().at(name);
    }
    void
    request::insert(httplib::field name, std::string_view value)
    {
        impl_->base().insert(enum_conv::to_field(name), value);
    }

    void
    request::insert(std::string_view name, std::string_view value)
    {
        impl_->base().insert(name, value);
    }

    void
    request::set(httplib::field name, std::string_view value)
    {
        impl_->base().set(enum_conv::to_field(name), value);
    }

    void
    request::set(std::string_view name, std::string_view value)
    {
        impl_->base().set(name, value);
    }

    void
    request::erase(httplib::field name)
    {
        impl_->base().erase(enum_conv::to_field(name));
    }

    void
    request::erase(std::string_view name)
    {
        impl_->base().erase(name);
    }

    bool
    request::has(httplib::field name) const
    {
        return impl_->base().find(enum_conv::to_field(name)) != impl_->base().end();
    }

    bool
    request::has(std::string_view name) const
    {
        return impl_->base().find(name) != impl_->base().end();
    }

    std::size_t
    request::count(httplib::field name) const
    {
        return impl_->base().count(enum_conv::to_field(name));
    }

    std::size_t
    request::count(std::string_view name) const
    {
        return impl_->base().count(name);
    }

    void
    request::replace(httplib::headers const& fields)
    {
        if (fields.empty())
        {
            return;
        }
        impl_->replace_fields(get_impl(fields));
    }

    httplib::headers
    request::base()
    {
        return httplib::headers::impl::borrow(impl_->base());
    }

    httplib::headers
    request::base() const
    {
        // shared_ptr 的 const 不传递到被指对象，impl_ 可变，视图直接借用消息的字段集合。
        return httplib::headers::impl::borrow(impl_->base());
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
        impl_->set_string(std::move(data), content_type);
    }

    void
    request::set_body(boost::json::value&& data)
    {
        impl_->set_json(std::move(data));
        impl_->prepare_payload();
    }

    void
    request::set_body(httplib::form_data&& data)
    {
        impl_->set_form_data(std::move(data));
        impl_->prepare_payload();
    }

    void
    request::set_body(httplib::query_params&& data)
    {
        impl_->set_query_params(std::move(data));
        impl_->prepare_payload();
    }

    void
    request::set_file_body(fs::path const& path, boost::system::error_code& ec)
    {
        std::ifstream file(path, std::ios::binary | std::ios::in);
        if (!file.is_open())
        {
            ec = boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory);
            return;
        }
        std::string content_type(mime::get_mime_type(path.extension().string()));
        impl_->set_file(std::move(file), content_type, html::http_ranges {});
    }

} // namespace httplib::client
