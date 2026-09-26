#pragma once

#include "body/body_writer.hpp"
#include "client_impl.h"
#include "httplib/client/lazy_request.hpp"
#include "response_impl.h"
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/beast/version.hpp>
#include <limits>

namespace httplib::client
{
    class http_client::impl::lazy_request_impl final
        : public lazy_request
        , public httplib::detail::body_writer<true, http_client::impl>
    {
      public:
        using body_writer_t = httplib::detail::body_writer<true, http_client::impl>;

        explicit lazy_request_impl(net::any_io_executor ex, std::shared_ptr<http_client::impl> parent)
            :parent_(std::move(parent))
        {
            attach(parent_.get(), std::move(ex));
        }
        net::awaitable<void>
        write_header(http::verb method, std::string_view target, http::fields const& headers, mode m) override
        {
            boost::system::error_code ec;
            co_await write_header(method, target, headers, m, ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }
        net::awaitable<void>
        write_header(http::verb method,
                     std::string_view target,
                     http::fields const& headers,
                     mode m,
                     boost::system::error_code& ec) override
        {
            if (!parent_)
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
                co_return;
            }

            this->reset();
            this->base().clear();
            this->base().method(method);
            this->base().target(target);
            this->base().version(11);
            this->base().set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
            for (auto const& f : headers)
            {
                this->base().set(f.name_string(), f.value());
            }
            this->base().set(http::field::host, parent_->host_value_);
            this->keep_alive(true);
            mode_ = m;
            // chunked 自带分帧；relay 保留调用方配置的分帧。
            if (m == mode::chunked)
            {
                this->chunked(true);
            }
            ec = co_await this->begin_stream();
        }

        net::awaitable<void>
        write_body(net::const_buffer const& data, bool more) override
        {
            boost::system::error_code ec;
            co_await write_body(data, more, ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }

        net::awaitable<void>
        write_body(net::const_buffer const& data, bool more, boost::system::error_code& ec) override
        {
            if (!parent_)
            {
                ec = boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
                co_return;
            }
            // relay 直通原始字节；chunked 由 body_writer 按 Content-Encoding 自动压缩。
            if (mode_ == mode::relay)
            {
                ec = co_await this->write_raw(data, more);
                co_return;
            }
            ec = co_await this->write_compressed(data, more);
        }

        net::awaitable<boost::system::result<client::response>>
        read_response_lazy() override
        {
            if (!parent_)
            {
                co_return boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
            }
            co_return co_await parent_->read_response_lazy(this->base().method());
        }

        net::awaitable<boost::system::result<client::response>>
        read_response() override
        {
            if (!parent_)
            {
                co_return boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
            }
            auto result = co_await read_response_lazy();
            if (result.has_error())
            {
                co_return result.error();
            }
            if (auto ec = co_await result->read_body(); ec)
            {
                co_return ec;
            }
            co_return result;
        }

      private:
        std::shared_ptr<http_client::impl> parent_;
        mode mode_ = mode::chunked;
    };
} // namespace httplib::client
