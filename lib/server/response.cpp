#include "httplib/server/response.hpp"
#include "html/html.h"
#include "response_impl.hpp"
#include "streaming/ndjson_writer_impl.hpp"
#include "streaming/sse_writer_impl.hpp"
#include "util/mime_types.hpp"
#include <boost/beast/version.hpp>
#include <fmt/format.h>

namespace httplib::server {

response::response(std::unique_ptr<impl>&& _impl)
    : impl_(std::move(_impl))
{
}

response::~response()
{
}
httplib::http::fields& response::base()
{
    return impl_->base();
}

const httplib::http::fields& response::base() const
{
    return impl_->base();
}
void response::set(http::field name, std::string_view value)
{
    impl_->set(name, value);
}

void response::set(std::string_view name, std::string_view value)
{
    impl_->set(name, value);
}

std::string_view response::operator[](http::field name) const
{
    return (*impl_)[name];
}

std::string_view response::operator[](std::string_view name) const
{
    return (*impl_)[name];
}

std::string_view response::at(http::field name) const
{
    return impl_->base().at(name);
}

std::string_view response::at(std::string_view name) const
{
    return impl_->base().at(name);
}

bool response::has(http::field name) const
{
    return impl_->find(name) != impl_->end();
}

bool response::has(std::string_view name) const
{
    return impl_->find(name) != impl_->end();
}

void response::erase(http::field name)
{
    impl_->erase(name);
}

void response::erase(std::string_view name)
{
    impl_->erase(name);
}

httplib::http::status response::result() const
{
    return impl_->result();
}
unsigned response::result_int() const
{
    return impl_->result_int();
}

void response::set_empty_content(http::status status)
{
    impl_->set_empty_content(status);
}

void response::set_error_content(http::status status)
{
    impl_->set_error_content(status);
}

void response::set_string_content(std::string&& data,
                                  std::string_view content_type,
                                  http::status status /*= http::status::ok*/)
{
    impl_->set_string_content(std::move(data), content_type, status);
}

void response::set_json_content(boost::json::value&& data,
                                http::status status /*= http::status::ok*/)
{
    impl_->set_json_content(std::move(data), status);
}

void response::set_file_content(const fs::path& path, const http::fields& req_header /*= {}*/)
{
    impl_->set_file_content(path, req_header);
}

void response::set_form_data_content(std::vector<html::form_data::field>&& data)
{
    impl_->set_form_data_content(std::move(data));
}

void response::set_redirect(std::string_view url,
                            http::status status /*= http::status::moved_permanently*/)
{
    impl_->set_redirect(url, status);
}

void response::set_chunked_write_handler(response::chunked_write_handler_type&& handler,
                                         std::string_view content_type,
                                         http::status status /*= http::status::ok*/)
{
    impl_->set_chunked_write_handler(std::move(handler), content_type, status);
}

void response::set_sse_write_handler(sse_write_handler_type&& handler)
{
    impl_->set_chunked_write_handler(
        [handler = std::move(handler)](chunk_writer& cw) -> net::awaitable<void> {
            streaming::sse_writer_impl sse(cw);
            co_await handler(sse);
        },
        "text/event-stream");
}

void response::set_ndjson_write_handler(ndjson_write_handler_type&& handler)
{
    impl_->set_chunked_write_handler(
        [handler = std::move(handler)](chunk_writer& cw) -> net::awaitable<void> {
            streaming::ndjson_writer_impl ndjson(cw);
            co_await handler(ndjson);
        },
        "application/x-ndjson");
}

httplib::server::response::impl* response::get_impl()
{
    return impl_.get();
}

const response::impl* response::get_impl() const
{
    return impl_.get();
}

} // namespace httplib::server