#pragma once
#include "body/body_state.hpp"
#include "body/body_writer.hpp"
#include "body/source.hpp"
#include "httplib/client/client.hpp"
#include "httplib/client/request.hpp"
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/message.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace httplib::client
{
    class request::impl : public http::request<http::buffer_body>
    {
      public:
        // 写方向的连接写入端是 http_client::impl（send 时 attach）。
        using body_writer_t = httplib::detail::body_writer<true, http_client::impl>;

        impl(http::verb method, std::string_view target, unsigned version)
            : http::request<http::buffer_body>(method, target, version)
            , writer_(*this)
        {
        }

        body_writer_t&
        writer()
        {
            return writer_;
        }

        void
        reset_body()
        {
            writer_.reset();
        }

        body::body_state&
        payload()
        {
            return writer_.payload();
        }

        body::body_state const&
        payload() const
        {
            return writer_.payload();
        }

      private:
        // 业务数据 + source + 序列化/编码/分帧状态统一收口在 body_writer。
        body_writer_t writer_;
    };
} // namespace httplib::client
