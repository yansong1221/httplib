#pragma once
#include "body/body_state.hpp"
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>
#include <utility>

namespace httplib::body
{
    /// 读取剩余 body 并按 Body 物化，返回 `result<Body::value_type>`（失败携带 error_code）。
    ///
    /// Reader 需提供：
    ///   using message_t;                       // 物化目标消息类型
    ///   using body_setup_fn;                   // std::function<void(message_t&)>
    ///   net::awaitable<void> read_body(body_setup_fn const&, error_code&);
    ///   body_state& body_state();
    ///
    /// Body 需显式指定（如 `read_as<string_body>(reader)`）。不传 setup 时按 Body 默认构造 body。
    template <typename Body, typename Reader>
    net::awaitable<boost::system::result<typename Body::value_type>>
    read_as(Reader& reader, typename Reader::body_setup_fn setup = {})
    {
        boost::system::error_code ec;
        if (!setup)
        {
            setup = [](typename Reader::message_t& msg)
            { msg.body() = typename Body::value_type {}; };
        }
        co_await reader.read_body(setup, ec);
        if (ec)
        {
            co_return ec;
        }
        co_return reader.body_state().template take<typename Body::value_type>();
    }
} // namespace httplib::body
