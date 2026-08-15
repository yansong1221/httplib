#pragma once
#include "httplib/mysql/mysql_exception.hpp"
#include "httplib/mysql/temporal.hpp"
#include <boost/mysql/field_view.hpp>
#include <string>

namespace httplib::mysql::util
{
    static std::string
    quote_mysql_string(std::string_view sv)
    {
        std::string out;
        out.reserve(sv.size() + 2);
        out += '\'';
        for (char c : sv)
        {
            // 反斜杠与单引号都要翻倍：MySQL 默认 NO_BACKSLASH_ESCAPES 关闭时 \ 是转义符，
            // 不转义反斜杠会吞掉闭合引号或把 \n/\b 等解释成控制字符。
            if (c == '\'' || c == '\\')
            {
                out += c;
                out += c;
            }
            else
            {
                out += c;
            }
        }
        out += '\'';
        return out;
    }

    static std::string
    format_param(boost::mysql::field_view const& f)
    {
        if (f.is_null())
        {
            return "NULL";
        }
        if (f.is_int64())
        {
            return std::to_string(f.as_int64());
        }
        if (f.is_uint64())
        {
            return std::to_string(f.as_uint64());
        }
        if (f.is_double())
        {
            return std::to_string(f.as_double());
        }
        if (f.is_string())
        {
            return quote_mysql_string(f.as_string());
        }
        if (f.is_blob())
        {
            static char const* digits = "0123456789ABCDEF";
            auto b = f.as_blob();
            std::string out;
            out.reserve(b.size() * 2 + 3);
            out += "X'";
            for (unsigned char byte : b)
            {
                out += digits[(byte >> 4) & 0xF];
                out += digits[byte & 0xF];
            }
            out += '\'';
            return out;
        }
        if (f.is_date())
        {
            auto d = f.as_date();
            return std::to_string(static_cast<int>(d.year())) + "-" + std::to_string(static_cast<int>(d.month())) + "-"
                   + std::to_string(static_cast<int>(d.day()));
        }
        if (f.is_datetime())
        {
            auto d = f.as_datetime();
            return datetime { d.year(), d.month(), d.day(), d.hour(), d.minute(), d.second(), d.microsecond() }
                .to_string();
        }
        if (f.is_time())
        {
            return time::from_duration(f.as_time()).to_string();
        }
        return "?";
    }

    static void
    raise_mysql_error(boost::system::error_code ec,
                      boost::mysql::diagnostics const& diag,
                      std::string_view sql,
                      std::string_view params)
    {
        if (!ec)
        {
            return;
        }
        auto what
            = std::string("[") + std::to_string(ec.value()) + "] " + ec.message() + " (SQL: " + std::string(sql) + ")";
        if (!params.empty())
        {
            what += " params: " + std::string(params);
        }
        auto msg = diag.server_message();
        if (!msg.empty())
        {
            what += ": " + std::string(msg.data(), msg.size());
        }
        throw mysql_exception(ec, what);
    }

    static void
    raise_mysql_error(boost::system::error_code ec, boost::mysql::diagnostics const& diag)
    {
        raise_mysql_error(ec, diag, {}, {});
    }
} // namespace httplib::mysql::util
