#pragma once

#include "backend.hpp"
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
                else if constexpr (std::is_same_v<T, std::span<std::byte const>>)
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
                else if constexpr (std::is_same_v<T, param_array>)
                {
                    std::string out = "[";
                    for (size_t i = 0; i < x.values.size(); ++i)
                    {
                        if (i > 0)
                        {
                            out += ", ";
                        }
                        out += format_param(x.values[i]);
                    }
                    out += "]";
                    return out;
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
    /// expanded 表示本次渲染是否发生数组展开（渲染出的 `?` 数量随数组长度变化，不可缓存）。
    struct rendered_query
    {
        std::string sql;
        std::vector<param> params;
        bool expanded = false;
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
                while (start < sql.size()
                       && (std::isalnum(static_cast<unsigned char>(sql[start])) || sql[start] == '_'))
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
     * \brief 把 SQL 中的 `:name` 占位符重写为后端占位符，并按出现顺序收集参数。
     * \details
     * 占位符文本由后端 \ref backend::placeholder(index, name) 提供，渲染层只维护编号（0 起递增）。
     * \n
     * 命名绑定按名字查表；位置绑定按占位符出现顺序消费。
     * \n
     * 数组参数（param 为 param_array）会被展开为多个占位符，例如 `IN (:ids)` 配 `[1,2,3]`
     * 渲染为 `IN (?,?,?)`（`?` 风格）或 `IN ($1,$2,$3)`（`$N` 风格）；空数组抛异常（无法表达空列表）。
     * \n
     * 每个占位符（含同名重复出现）都独立编号并收集一次值，后端决定编号如何映射为文本。
     */
    inline rendered_query
    render_query(std::string_view sql, std::vector<binder> const& binders, backend const& b)
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
                // 重复绑定同名参数：与 prepared_statement 一致，后者生效。
                named[b.name] = &b.value;
            }
        }
        if (has_named && has_pos)
        {
            throw std::runtime_error("db: cannot mix positional and named parameters");
        }

        std::string out;
        out.reserve(sql.size() + 16);
        std::vector<param> params;
        bool expanded = false;
        size_t ph = 0; ///< 下一个占位符编号（0 起）

        auto emit_placeholder = [&](param const* value, std::string_view label)
        {
            if (auto* arr = std::get_if<param_array>(value))
            {
                if (arr->values.empty())
                {
                    std::string what = "db: empty array parameter";
                    if (!label.empty())
                    {
                        what += " ':" + std::string(label) + "'";
                    }
                    throw std::runtime_error(what);
                }
                for (size_t k = 0; k < arr->values.size(); ++k)
                {
                    if (k > 0)
                    {
                        out += ',';
                    }
                    out += b.placeholder(ph++, label);
                    params.push_back(arr->values[k]);
                }
                expanded = true;
            }
            else
            {
                out += b.placeholder(ph++, label);
                params.push_back(*value);
            }
        };

        size_t pos_idx = 0;
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
                while (start < sql.size()
                       && (std::isalnum(static_cast<unsigned char>(sql[start])) || sql[start] == '_'))
                {
                    ++start;
                }
                if (start > i + 1)
                {
                    std::string name(sql.substr(i + 1, start - i - 1));
                    if (has_named)
                    {
                        auto it = named.find(name);
                        if (it == named.end())
                        {
                            throw std::runtime_error("db: unbound named parameter ':" + name + "'");
                        }
                        // 每处出现独立编号并收集值（`?` 无法引用同一参数；`$N` 风格下多占位符同值也正确）。
                        emit_placeholder(it->second, name);
                    }
                    else if (pos.empty())
                    {
                        // 没有位置绑定但 SQL 存在占位符 → 未绑定（与命名未绑定语义一致）。
                        throw std::runtime_error("db: unbound named parameter ':" + name + "'");
                    }
                    else
                    {
                        if (pos_idx >= pos.size())
                        {
                            // 参数数量少于占位符：与后端 prepare 的 arity 检查一致，视为数据库层错误。
                            throw db_exception(boost::system::error_code {}, "db: too few parameters for placeholders");
                        }
                        emit_placeholder(pos[pos_idx++], {});
                    }
                    i = start - 1;
                    continue;
                }
            }
            out += c;
        }
        if (!has_named && pos_idx != pos.size())
        {
            // 位置参数多于占位符：与后端 prepare 的 arity 检查一致，视为数据库层错误。
            throw db_exception(boost::system::error_code {}, "db: too many parameters for placeholders");
        }
        return { std::move(out), std::move(params), expanded };
    }

} // namespace httplib::db::detail
