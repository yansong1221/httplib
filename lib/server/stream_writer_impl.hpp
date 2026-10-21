#pragma once
#include "beast_alias.hpp"
#include "enum_conv.hpp"
#include "httplib/server/stream_writer.hpp"
#include "response_impl.hpp"
#include <boost/asio/error.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/system/error_code.hpp>
#include <memory>
#include <string>

namespace httplib::server
{

    class stream_writer_impl : public stream_writer
    {
      public:
        explicit stream_writer_impl(std::shared_ptr<response::impl> resp) : resp_(std::move(resp)) {}

        net::awaitable<void>
        write_header(httplib::status status, httplib::headers const& headers, mode m) override
        {
            boost::system::error_code ec;
            co_await write_header(status, headers, m, ec);
            if (ec)
            {
                throw boost::system::system_error(ec);
            }
        }
        net::awaitable<void>
        write_header(httplib::status status,
                     httplib::headers const& headers,
                     mode m,
                     boost::system::error_code& ec) override
        {
            // 整个 co_await 期间持有 impl：即使 response 在挂起期间析构，begin_stream
            // 也不会踩到已释放对象；连接关闭由 task/stream 侧以 ec 收口。
            auto resp = resp_.lock();
            if (!resp)
            {
                ec = net::error::operation_aborted;
                co_return;
            }

            resp->base().result(enum_conv::to_status(status));
            resp->replace_fields(headers);

            mode_ = m;
            // chunked 自带分帧；relay 保留上游原有分帧（Content-Length / Transfer-Encoding）。
            if (m == mode::chunked)
            {
                resp->keep_alive(true);
                resp->chunked(true);
            }
            ec = co_await resp->writer().begin_stream();
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
            auto resp = resp_.lock();
            if (!resp)
            {
                ec = net::error::operation_aborted;
                co_return;
            }

            // relay 直通原始字节；chunked 由 body_writer 按 Content-Encoding 自动压缩。
            if (mode_ == mode::relay)
            {
                ec = co_await resp->writer().write_raw(data, more);
            }
            else
            {
                ec = co_await resp->writer().write_encoded(data, more);
            }
        }

      private:
        // 只弱持有 response：writer 被带出 handler 时不会悬垂，越界写返回
        // operation_aborted，而不是踩已释放的 response::impl。
        std::weak_ptr<response::impl> resp_;
        mode mode_ = mode::chunked;
    };

} // namespace httplib::server
