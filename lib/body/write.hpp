#pragma once
#include "body/source.hpp"
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/serializer.hpp>
#include <boost/system/error_code.hpp>

namespace httplib::body
{
    /** 用 source 驱动 `http::buffer_body` 序列化器写出整个消息。

        注意：Beast 的 `http::buffer_body::writer` 只接受 const 值引用，故 serializer 的
        `value_type` 是 const message —— 不能经 `serializer.get()` 改 body，必须改调用方持有的
        原始 message body（@p body）。每次序列化器要新缓冲（`need_buffer`）时向 source 取下一块
        填进去；source 取尽则以终结块收尾。write_some 形如
        `write_some(serializer, ec) -> awaitable<void>`。
    */
    template <bool IsRequest, typename WriteSome>
    net::awaitable<void>
    write_message(http::serializer<IsRequest, http::buffer_body, http::fields>* serializer,
                  http::buffer_body::value_type* body,
                  source* src,
                  WriteSome write_some,
                  boost::system::error_code& ec)
    {
        static char empty_byte = 0;
        bool refill = true;
        while (!serializer->is_done())
        {
            if (refill)
            {
                source::chunk_t chunk;
                if (src)
                {
                    chunk = src->next(ec);
                    if (ec)
                    {
                        co_return;
                    }
                }
                if (chunk)
                {
                    body->data = const_cast<void*>(chunk->first.data());
                    body->size = chunk->first.size();
                    body->more = chunk->second;
                }
                else
                {
                    body->data = &empty_byte;
                    body->size = 0;
                    body->more = false;
                }
            }

            co_await write_some(*serializer, ec);
            if (ec == http::error::need_buffer)
            {
                ec = {};
                refill = true;
            }
            else if (ec)
            {
                co_return;
            }
            else
            {
                refill = false;
            }
        }
        ec = {};
    }

} // namespace httplib::body
