#include "httplib/server/response.hpp"
#include "html/html.h"
#include "headers_impl.hpp"
#include "ndjson_writer_impl.hpp"
#include "response_impl.hpp"
#include "sse_writer_impl.hpp"
#include "stream_writer_impl.hpp"
#include "util/mime_types.hpp"
#include <boost/beast/version.hpp>
#include <fmt/format.h>
#include "beast_alias.hpp"
#include "enum_conv.hpp"

namespace httplib::server
{

    response::response(std::unique_ptr<impl>&& _impl) : impl_(std::move(_impl)) {}

    response::~response() {}

    httplib::headers
    response::base()
    {
        return httplib::detail::headers_access::borrow(impl_->base());
    }

    httplib::headers
    response::base() const
    {
        // unique_ptr 的 const 不传递到被指对象，故这里无需 const_cast：
        // 借用视图本身就指向消息自己的字段集合，写入语义与非常量重载一致。
        return httplib::detail::headers_access::borrow(impl_->base());
    }

    void
    response::set(httplib::field name, std::string_view value)
    {
        impl_->base().set(enum_conv::to_field(name), value);
    }

    void
    response::set(std::string_view name, std::string_view value)
    {
        impl_->base().set(name, value);
    }

    void
    response::insert(httplib::field name, std::string_view value)
    {
        impl_->base().insert(enum_conv::to_field(name), value);
    }

    void
    response::insert(std::string_view name, std::string_view value)
    {
        impl_->base().insert(name, value);
    }

    std::string_view
    response::operator[](httplib::field name) const
    {
        return impl_->base()[enum_conv::to_field(name)];
    }

    std::string_view
    response::operator[](std::string_view name) const
    {
        return impl_->base()[name];
    }

    std::string_view
    response::at(httplib::field name) const
    {
        return impl_->base().at(enum_conv::to_field(name));
    }

    std::string_view
    response::at(std::string_view name) const
    {
        return impl_->base().at(name);
    }

    bool
    response::has(httplib::field name) const
    {
        return impl_->base().find(enum_conv::to_field(name)) != impl_->base().end();
    }

    bool
    response::has(std::string_view name) const
    {
        return impl_->base().find(name) != impl_->base().end();
    }

    void
    response::erase(httplib::field name)
    {
        impl_->base().erase(enum_conv::to_field(name));
    }

    void
    response::erase(std::string_view name)
    {
        impl_->base().erase(name);
    }

    std::size_t
    response::count(httplib::field name) const
    {
        return impl_->base().count(enum_conv::to_field(name));
    }

    std::size_t
    response::count(std::string_view name) const
    {
        return impl_->base().count(name);
    }

    httplib::status
    response::result() const
    {
        return enum_conv::to_status(impl_->base().result());
    }

    unsigned
    response::result_int() const
    {
        return impl_->base().result_int();
    }

    void
    response::set_empty_content(httplib::status status)
    {
        impl_->set_empty_content(enum_conv::to_status(status));
    }

    void
    response::set_error_content(httplib::status status)
    {
        impl_->set_error_content(enum_conv::to_status(status));
    }

    void
    response::set_string_content(std::string&& data, std::string_view content_type, httplib::status status)
    {
        impl_->set_string_content(std::move(data), content_type, enum_conv::to_status(status));
    }

    void
    response::set_json_content(boost::json::value const& data, httplib::status status)
    {
        set_json_content(boost::json::value(data), status);
    }

    void
    response::set_json_content(boost::json::value&& data, httplib::status status)
    {
        impl_->set_json_content(std::move(data), enum_conv::to_status(status));
    }

    void
    response::set_file_content(fs::path const& path, httplib::headers const& req_header)
    {
        impl_->set_file_content(path, httplib::detail::headers_access::raw(req_header));
    }

    void
    response::set_form_data_content(std::vector<httplib::form_data::field>&& data)
    {
        impl_->set_form_data_content(std::move(data));
    }

    void
    response::set_redirect(std::string_view url, httplib::status status)
    {
        impl_->set_redirect(url, enum_conv::to_status(status));
    }

    std::unique_ptr<server::sse_writer>
    response::create_sse_writer()
    {
        return std::make_unique<sse_writer_impl>(create_stream_writer());
    }

    std::unique_ptr<server::ndjson_writer>
    response::create_ndjson_writer()
    {
        return std::make_unique<ndjson_writer_impl>(create_stream_writer());
    }

    stream_writer*
    response::create_stream_writer()
    {
        if (!impl_->stream_writer_)
        {
            impl_->stream_writer_ = std::make_unique<stream_writer_impl>(*impl_);
        }
        return impl_->stream_writer_.get();
    }

} // namespace httplib::server
