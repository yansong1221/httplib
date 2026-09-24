#pragma once
#include "body/body_state.hpp"
#include "body/source.hpp"
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
        impl(http::verb method, std::string_view target, unsigned version)
            : http::request<http::buffer_body>(method, target, version)
        {
        }

        body::source*
        source()
        {
            return source_.get();
        }

        void
        set_source(body::source_ptr source)
        {
            source_ = std::move(source);
        }

        void
        reset_body()
        {
            payload_.reset();
            source_.reset();
            this->body() = http::buffer_body::value_type {};
        }

        /// 按 Content-Encoding 在现有 source 上叠加编码（压缩）。
        void
        apply_encoding(std::string_view encoding)
        {
            if (!source_)
            {
                source_ = std::make_unique<body::empty_source>();
            }
            source_ = std::make_unique<body::encoded_source>(std::move(source_), encoding);
        }

        body::body_state&
        payload()
        {
            return payload_;
        }

        body::body_state const&
        payload() const
        {
            return payload_;
        }

      private:
        // 业务数据（字符串 / json / form_data / query_params），source_ 引用它产出字节。
        body::body_state payload_;
        body::source_ptr source_;
    };
} // namespace httplib::client
