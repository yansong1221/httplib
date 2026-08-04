#include "httplib/server/response.hpp"
#include "chunk_writer_impl.hpp"
#include "html/html.h"
#include "ndjson_writer_impl.hpp"
#include "response_impl.hpp"
#include "sse_writer_impl.hpp"
#include "util/mime_types.hpp"
#include <boost/beast/version.hpp>
#include <fmt/format.h>

namespace httplib::server
{

    response::response(std::unique_ptr<impl>&& _impl) : impl_(std::move(_impl)) {}

    response::~response() {}
    httplib::http::fields&
    response::base()
    {
        return impl_->base();
    }

    httplib::http::fields const&
    response::base() const
    {
        return impl_->base();
    }
    void
    response::set(http::field name, std::string_view value)
    {
        impl_->set(name, value);
    }

    void
    response::set(std::string_view name, std::string_view value)
    {
        impl_->set(name, value);
    }

    std::string_view
    response::operator[](http::field name) const
    {
        return (*impl_)[name];
    }

    std::string_view
    response::operator[](std::string_view name) const
    {
        return (*impl_)[name];
    }

    std::string_view
    response::at(http::field name) const
    {
        return impl_->base().at(name);
    }

    std::string_view
    response::at(std::string_view name) const
    {
        return impl_->base().at(name);
    }

    bool
    response::has(http::field name) const
    {
        return impl_->find(name) != impl_->end();
    }

    bool
    response::has(std::string_view name) const
    {
        return impl_->find(name) != impl_->end();
    }

    void
    response::erase(http::field name)
    {
        impl_->erase(name);
    }

    void
    response::erase(std::string_view name)
    {
        impl_->erase(name);
    }

    httplib::http::status
    response::result() const
    {
        return impl_->result();
    }
    unsigned
    response::result_int() const
    {
        return impl_->result_int();
    }

    void
    response::set_empty_content(http::status status)
    {
        impl_->set_empty_content(status);
    }

    void
    response::set_error_content(http::status status)
    {
        impl_->set_error_content(status);
    }

    void
    response::set_string_content(std::string&& data,
                                 std::string_view content_type,
                                 http::status status /*= http::status::ok*/)
    {
        impl_->set_string_content(std::move(data), content_type, status);
    }

    void
    response::set_json_content(boost::json::value const& data, http::status status)
    {
        set_json_content(boost::json::value(data), status);
    }

    void
    response::set_json_content(boost::json::value&& data, http::status status /*= http::status::ok*/)
    {
        impl_->set_json_content(std::move(data), status);
    }

    void
    response::set_file_content(fs::path const& path, http::fields const& req_header /*= {}*/)
    {
        impl_->set_file_content(path, req_header);
    }

    void
    response::set_form_data_content(std::vector<html::form_data::field>&& data)
    {
        impl_->set_form_data_content(std::move(data));
    }

    void
    response::set_redirect(std::string_view url, http::status status /*= http::status::moved_permanently*/)
    {
        impl_->set_redirect(url, status);
    }

    std::unique_ptr<server::sse_writer>
    response::create_sse_writer()
    {
        return std::make_unique<sse_writer_impl>(get_chunk_writer());
    }

    std::unique_ptr<server::ndjson_writer>
    response::create_ndjson_writer()
    {
        return std::make_unique<ndjson_writer_impl>(get_chunk_writer());
    }

    chunk_writer*
    response::get_chunk_writer()
    {
        if (!impl_->chunk_writer_)
        {
            impl_->chunk_writer_ = std::make_unique<chunk_writer_impl>(*impl_, *impl_->stream_, impl_->write_timeout_);
        }
        return impl_->chunk_writer_.get();
    }

    bool
    response::is_chunked_done() const
    {
        return impl_->chunk_writer_ && impl_->chunk_writer_->has_header();
    }

} // namespace httplib::server