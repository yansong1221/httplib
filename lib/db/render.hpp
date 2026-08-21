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
                else if constexpr (std::is_same_v<T, text>)
                {
                    return quote_string(x.data());
                }
                else if constexpr (std::is_same_v<T, blob>)
                {
                    static char const* digits = "0123456789ABCDEF";
                    std::string out;
                    out.reserve(x.size() * 2 + 3);
                    out += "X'";
                    for (std::byte b : x.data())
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
                else if constexpr (std::is_same_v<T, timestamp>)
                {
                    return quote_string(datetime::from_time_point(x).to_string());
                }
                else
                {
                    static_assert(std::is_same_v<T, timestamp>, "db: unhandled field type in format_param");
                    return {};
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

    /// 引号串原样复制到 out，返回串末尾索引（`'abc'` 返回结尾 `'`）。
    inline size_t
    copy_quoted(std::string& out, std::string_view sql, size_t i)
    {
        char quote = sql[i];
        out += quote;
        while (++i < sql.size())
        {
            if (sql[i] == quote)
            {
                if (i + 1 < sql.size() && sql[i + 1] == quote)
                {
                    out += quote; // '' 转义：成对引号原样输出
                    out += quote;
                    ++i; // 跳到第二个引号，循环 ++i 越过
                    continue;
                }
                out += quote;
                break; // 闭合引号
            }
            out += sql[i];
            if (sql[i] == '\\' && i + 1 < sql.size())
            {
                out += sql[++i];
            }
        }
        return i;
    }

    /// 行注释（`#` 或 `--`）复制到 out（保留换行符），返回换行符索引。
    inline size_t
    copy_line_comment(std::string& out, std::string_view sql, size_t i)
    {
        while (i < sql.size() && sql[i] != '\n')
        {
            ++i;
        }
        if (i < sql.size())
        {
            out += sql[i];
        }
        return i;
    }

    /// 块注释 `/* ... */` 跳过，返回 `*/` 的 `/` 索引。
    inline size_t
    copy_block_comment(std::string_view sql, size_t i)
    {
        i += 2;
        while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/'))
        {
            ++i;
        }
        return i + 1;
    }

    /// 绑定索引：命名表（同名后者生效）与位置序列。
    struct binder_index
    {
        bool has_named = false;
        bool has_pos = false;
        std::unordered_map<std::string, param const*> named;
        std::vector<param const*> pos;
    };

    /// 把绑定列表分类为命名表与位置序列。
    inline binder_index
    build_binder_index(std::vector<binder> const& binders)
    {
        binder_index idx;
        for (auto const& b : binders)
        {
            if (b.name.empty())
            {
                idx.has_pos = true;
                idx.pos.push_back(&b.value);
            }
            else
            {
                idx.has_named = true;
                // 重复绑定同名参数：与 prepared_statement 一致，后者生效。
                idx.named[b.name] = &b.value;
            }
        }
        return idx;
    }

    /// 解析 `:name` 绑定：命名查表，位置按序消费。
    inline param const*
    resolve_param(std::string const& name,
                  std::unordered_map<std::string, param const*> const& named,
                  std::vector<param const*> const& pos,
                  size_t& pos_idx,
                  bool has_named)
    {
        if (has_named)
        {
            auto it = named.find(name);
            if (it == named.end())
            {
                throw db_exception(boost::system::error_code {}, "db: unbound named parameter ':" + name + "'");
            }
            return it->second;
        }
        if (pos.empty())
        {
            // 没有位置绑定但 SQL 存在占位符 → 未绑定（与命名未绑定语义一致）。
            throw db_exception(boost::system::error_code {}, "db: unbound named parameter ':" + name + "'");
        }
        if (pos_idx >= pos.size())
        {
            // 参数数量少于占位符：与后端 prepare 的 arity 检查一致，视为数据库层错误。
            throw db_exception(boost::system::error_code {}, "db: too few parameters for placeholders");
        }
        return pos[pos_idx++];
    }

    /// 输出单个占位符并收集参数。
    inline void
    emit_placeholder(std::string& out,
                     std::vector<param>& params,
                     size_t& ph,
                     backend const& b,
                     param const* value,
                     std::string_view label)
    {
        out += b.placeholder(ph++, label);
        params.push_back(*value);
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
                i = copy_quoted(out, sql, i);
                continue;
            }
            if (c == '#')
            {
                i = copy_line_comment(out, sql, i);
                continue;
            }
            if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-'
                && (i + 2 >= sql.size() || std::isspace(static_cast<unsigned char>(sql[i + 2]))))
            {
                i = copy_line_comment(out, sql, i);
                continue;
            }
            if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*')
            {
                i = copy_block_comment(sql, i);
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
     * 每个占位符（含同名重复出现）都独立编号并收集一次值，后端决定编号如何映射为文本。
     */
    inline rendered_query
    render_query(std::string_view sql, std::vector<binder> const& binders, backend const& b)
    {
        binder_index idx = build_binder_index(binders);
        if (idx.has_named && idx.has_pos)
        {
            throw db_exception(boost::system::error_code {}, "db: cannot mix positional and named parameters");
        }

        std::string out;
        out.reserve(sql.size() + 16);
        std::vector<param> params;
        size_t ph = 0; ///< 下一个占位符编号（0 起）

        size_t pos_idx = 0;
        for (size_t i = 0; i < sql.size(); ++i)
        {
            char c = sql[i];
            if (c == '\'' || c == '"' || c == '`')
            {
                i = copy_quoted(out, sql, i);
                continue;
            }
            if (c == '#')
            {
                i = copy_line_comment(out, sql, i);
                continue;
            }
            if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-'
                && (i + 2 >= sql.size() || std::isspace(static_cast<unsigned char>(sql[i + 2]))))
            {
                i = copy_line_comment(out, sql, i);
                continue;
            }
            if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*')
            {
                i = copy_block_comment(sql, i);
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
                    param const* p = resolve_param(name, idx.named, idx.pos, pos_idx, idx.has_named);
                    emit_placeholder(out, params, ph, b, p, name);
                    i = start - 1;
                    continue;
                }
            }
            out += c;
        }
        if (!idx.has_named && pos_idx != idx.pos.size())
        {
            // 位置参数多于占位符：与后端 prepare 的 arity 检查一致，视为数据库层错误。
            throw db_exception(boost::system::error_code {}, "db: too many parameters for placeholders");
        }
        return { std::move(out), std::move(params) };
    }

    /// 按语句级分号拆分 SQL（跳过字符串/引号/注释内的分号，跳过空语句）。
    /// 供 execute_rendered 检测参数化多语句（含绑定参数的语句不允许多语句）。
    inline std::vector<std::string_view>
    split_statements(std::string_view sql)
    {
        std::vector<std::string_view> out;
        size_t const n = sql.size();
        size_t i = 0;
        while (i < n)
        {
            while (i < n && std::isspace(static_cast<unsigned char>(sql[i])))
            {
                ++i;
            }
            if (i >= n)
            {
                break;
            }
            size_t start = i;
            while (i < n)
            {
                char c = sql[i];
                if (c == '\'' || c == '"' || c == '`')
                {
                    char quote = c;
                    ++i;
                    while (i < n)
                    {
                        if (sql[i] == quote)
                        {
                            if (i + 1 < n && sql[i + 1] == quote)
                            {
                                i += 2; // '' 转义：跳过成对引号
                                continue;
                            }
                            ++i;
                            break; // 闭合引号
                        }
                        if (sql[i] == '\\' && i + 1 < n)
                        {
                            i += 2;
                        }
                        else
                        {
                            ++i;
                        }
                    }
                    continue;
                }
                if (c == '#')
                {
                    while (i < n && sql[i] != '\n')
                    {
                        ++i;
                    }
                    continue;
                }
                if (c == '-' && i + 1 < n && sql[i + 1] == '-'
                    && (i + 2 >= n || std::isspace(static_cast<unsigned char>(sql[i + 2]))))
                {
                    while (i < n && sql[i] != '\n')
                    {
                        ++i;
                    }
                    continue;
                }
                if (c == '/' && i + 1 < n && sql[i + 1] == '*')
                {
                    i += 2;
                    while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/'))
                    {
                        ++i;
                    }
                    i += 2; // 越过 */
                    continue;
                }
                if (c == ';')
                {
                    break;
                }
                ++i;
            }
            size_t end = i;
            while (end > start && std::isspace(static_cast<unsigned char>(sql[end - 1])))
            {
                --end;
            }
            if (end > start)
            {
                out.push_back(sql.substr(start, end - start));
            }
            if (i < n && sql[i] == ';')
            {
                ++i;
            }
        }
        return out;
    }

} // namespace httplib::db::detail
