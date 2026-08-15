#pragma once

#include "httplib/db/binder.hpp"
#include "httplib/db/exception.hpp"
#include <cctype>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace httplib::db::detail
{

    /// 单引号包裹并转义（错误消息里的人类可读文本）。
    inline std::string
    quote_string(std::string_view sv)
    {
        std::string out;
        out.reserve(sv.size() + 2);
        out += '\'';
        for (char c : sv)
        {
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

    /// 渲染单个参数为可读文本（错误消息用）。
    inline std::string
    format_param(param const& v)
    {
        return std::visit(
            [](auto const& x) -> std::string
            {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, std::monostate>)
                {
                    return "NULL";
                }
                else if constexpr (std::is_same_v<T, int64_t>)
                {
                    return std::to_string(x);
                }
                else if constexpr (std::is_same_v<T, uint64_t>)
                {
                    return std::to_string(x);
                }
                else if constexpr (std::is_same_v<T, double>)
                {
                    return std::to_string(x);
                }
                else if constexpr (std::is_same_v<T, std::string>)
                {
                    return quote_string(x);
                }
                else if constexpr (std::is_same_v<T, std::span<const std::byte>>)
                {
                    static char const* digits = "0123456789ABCDEF";
                    std::string out;
                    out.reserve(x.size() * 2 + 3);
                    out += "X'";
                    for (std::byte b : x)
                    {
                        auto u = static_cast<unsigned char>(b);
                        out += digits[(u >> 4) & 0xF];
                        out += digits[u & 0xF];
                    }
                    out += '\'';
                    return out;
                }
                else if constexpr (std::is_same_v<T, date>)
                {
                    return quote_string(x.to_string());
                }
                else if constexpr (std::is_same_v<T, datetime>)
                {
                    return quote_string(x.to_string());
                }
                else if constexpr (std::is_same_v<T, time>)
                {
                    return quote_string(x.to_string());
                }
                else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>)
                {
                    return quote_string(datetime::from_time_point(x).to_string());
                }
                else
                {
                    return "?";
                }
            },
            v);
    }

    /**
     * \brief 增强后端抛出的 db_exception：附加原始 SQL 与参数渲染。
     * \details 后端拿到的是重写后的 `?` SQL，这里补回原始 `:name` 形式与 `name=value` 参数，
     *          便于定位问题；保留原 error_code。
     */
    inline db_exception
    enrich_error(db_exception const& ex,
                 std::string_view original_sql,
                 std::vector<std::string> const& names,
                 std::vector<param> const& params)
    {
        std::string what = ex.what();
        what += " (original: " + std::string(original_sql) + ") params: [";
        for (size_t i = 0; i < params.size(); ++i)
        {
            if (i > 0)
            {
                what += ", ";
            }
            if (i < names.size() && !names[i].empty())
            {
                what += names[i];
                what += "=";
            }
            what += format_param(params[i]);
        }
        what += "]";
        return db_exception(ex.code(), what);
    }

    /// 渲染结果：重写后的 `?` SQL 与按占位符顺序排列的参数。
    struct rendered_query
    {
        std::string sql;
        std::vector<param> params;
    };

    /// 解析 `:name` 占位符：把 SQL 重写为 `?`，并按出现顺序返回占位符名字列表。
    inline std::pair<std::string, std::vector<std::string>>
    parse_placeholders(std::string_view sql)
    {
        std::string out;
        out.reserve(sql.size());
        std::vector<std::string> names;

        for (size_t i = 0; i < sql.size(); ++i)
        {
            char c = sql[i];
            if (c == '\'' || c == '"' || c == '`')
            {
                char quote = c;
                out += c;
                while (++i < sql.size())
                {
                    out += sql[i];
                    if (sql[i] == quote && (i + 1 >= sql.size() || sql[i + 1] != quote))
                    {
                        break;
                    }
                    if (sql[i] == '\\' && i + 1 < sql.size())
                    {
                        out += sql[++i];
                    }
                }
                continue;
            }
            if (c == '#')
            {
                while (i < sql.size() && sql[i] != '\n')
                {
                    ++i;
                }
                if (i < sql.size())
                {
                    out += sql[i];
                }
                continue;
            }
            if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-'
                && (i + 2 >= sql.size() || std::isspace(static_cast<unsigned char>(sql[i + 2]))))
            {
                while (i < sql.size() && sql[i] != '\n')
                {
                    ++i;
                }
                if (i < sql.size())
                {
                    out += sql[i];
                }
                continue;
            }
            if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*')
            {
                i += 2;
                while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/'))
                {
                    ++i;
                }
                i += 1;
                continue;
            }
            if (c == ':')
            {
                size_t start = i + 1;
                while (start < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[start])) || sql[start] == '_'))
                {
                    ++start;
                }
                if (start > i + 1)
                {
                    names.emplace_back(sql.substr(i + 1, start - i - 1));
                    out += '?';
                    i = start - 1;
                    continue;
                }
            }
            out += c;
        }
        return { std::move(out), std::move(names) };
    }

    /**
     * \brief 把 SQL 中的 `:name` 占位符重写为 `?`，并按出现顺序收集参数。
     * \details
     * 后端无关：MySQL / SQLite 都使用 `?` 作为绑定占位符。
     * \n
     * 命名绑定按名字查表；位置绑定按占位符出现顺序消费。
     */
    inline rendered_query
    render_query(std::string_view sql, std::vector<binder> const& binders)
    {
        bool has_named = false;
        bool has_pos = false;
        std::unordered_map<std::string, param const*> named;
        std::vector<param const*> pos;
        for (auto const& b : binders)
        {
            if (b.name.empty())
            {
                has_pos = true;
                pos.push_back(&b.value);
            }
            else
            {
                has_named = true;
                named.emplace(b.name, &b.value);
            }
        }
        if (has_named && has_pos)
        {
            throw std::runtime_error("db: cannot mix positional and named parameters");
        }

        auto [rewritten, names] = parse_placeholders(sql);

        rendered_query out;
        out.sql = std::move(rewritten);
        out.params.reserve(names.size());
        size_t pos_idx = 0;
        for (auto const& name : names)
        {
            if (has_named)
            {
                auto it = named.find(name);
                if (it == named.end())
                {
                    throw std::runtime_error("db: unbound named parameter ':" + name + "'");
                }
                out.params.push_back(*it->second);
            }
            else
            {
                if (pos_idx >= pos.size())
                {
                    throw std::runtime_error("db: too few parameters for placeholders");
                }
                out.params.push_back(*pos[pos_idx++]);
            }
        }
        if (!has_named && pos_idx != pos.size())
        {
            throw std::runtime_error("db: too many parameters");
        }
        return out;
    }

} // namespace httplib::db::detail
