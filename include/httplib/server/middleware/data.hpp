#pragma once
#include "httplib/server/request.hpp"
#include <any>
#include <optional>

namespace httplib::server::middleware
{

    /// \brief 取出 MW 存入的值；未存入时返回空 optional。
    ///
    /// 只提供 const 版本：取值的唯一方式是拷贝，单次调用内只加一次锁，因此对
    /// const / 非 const 的 request 表现一致。用 optional 而非抛异常，是因为这里
    /// "还没存" 是常态而非错误，调用方通常只是想探测。
    template <typename MW>
    std::optional<typename MW::value_type>
    fetch(request const& req, std::string_view tag = {})
    {
        return req.data().template fetch<typename MW::value_type>(tag);
    }

    /// \brief 判断 MW 是否已存入；不取值。
    ///
    /// 仅适合纯谓词用途。需要取值时用 fetch()（返回 optional），不要先 has() 再取值。
    template <typename MW>
    bool
    has(request const& req, std::string_view tag = {})
    {
        return req.data().template has<typename MW::value_type>(tag);
    }

    template <typename MW>
    void
    store(request& req, std::any val, std::string_view tag = {})
    {
        req.data().template store<typename MW::value_type>(tag, std::any_cast<typename MW::value_type>(std::move(val)));
    }

    template <typename MW>
    void
    erase(request& req, std::string_view tag = {})
    {
        req.data().erase<typename MW::value_type>(tag);
    }

} // namespace httplib::server::middleware
