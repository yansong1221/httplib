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

    /// 括号分组：is_values 表示该组是 `VALUES (` 列表；组内占位符按"列"收集，闭合时按列展开成多行。
    struct values_group_ctx
    {
        bool is_values = false;
        bool has_array = false;
        bool emitted = false;                   ///< 组内已原样输出内容（字面量/标量），其后不能再出现数组列
        std::vector<param const*> pending;      ///< 组内待展开的占位符（每项一列）
        std::vector<std::string> pending_names; ///< 与 pending 对应的名字（供错误信息）
    };

    /// 引号串原样复制到 out，返回串末尾索引（`'abc'` 返回结尾 `'`）。
    inline size_t
    copy_quoted(std::string& out, std::string_view sql, size_t i)
    {
        char quote = sql[i];
        out += quote;
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
                throw std::runtime_error("db: unbound named parameter ':" + name + "'");
            }
            return it->second;
        }
        if (pos.empty())
        {
            // 没有位置绑定但 SQL 存在占位符 → 未绑定（与命名未绑定语义一致）。
            throw std::runtime_error("db: unbound named parameter ':" + name + "'");
        }
        if (pos_idx >= pos.size())
        {
            // 参数数量少于占位符：与后端 prepare 的 arity 检查一致，视为数据库层错误。
            throw db_exception(boost::system::error_code {}, "db: too few parameters for placeholders");
        }
        return pos[pos_idx++];
    }

    /// 输出单个占位符：数组参数同一括号内平铺 `?,?,?`（`IN (:ids)` 等非 VALUES 场景），
    /// 标量输出一个占位符。返回是否发生数组展开。
    inline bool
    emit_placeholder(std::string& out,
                     std::vector<param>& params,
                     size_t& ph,
                     backend const& b,
                     param const* value,
                     std::string_view label)
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
            return true;
        }
        out += b.placeholder(ph++, label);
        params.push_back(*value);
        return false;
    }

    /**
     * \brief VALUES 组闭合时按"列"展开为多行值列表。
     * \details 每个占位符表示一列的所有数据：标量列（1 行）或数组列（多行）等长，
     *          生成 `(col1,col2,...)` 行列表，行间用 `),(` 分隔，最后一行的 `)` 由调用方补上。
     */
    inline void
    expand_values_group(std::string& out,
                        std::vector<param>& params,
                        size_t& ph,
                        backend const& b,
                        values_group_ctx const& g)
    {
        size_t rows = 1;
        for (auto* p : g.pending)
        {
            if (auto* arr = std::get_if<param_array>(p))
            {
                if (arr->values.empty())
                {
                    throw std::runtime_error("db: empty array parameter in VALUES");
                }
                rows = arr->values.size();
                break;
            }
        }
        for (auto* p : g.pending)
        {
            if (auto* arr = std::get_if<param_array>(p))
            {
                if (arr->values.size() != rows)
                {
                    throw db_exception(boost::system::error_code {}, "db: VALUES columns must have equal length");
                }
            }
            else if (rows != 1)
            {
                throw db_exception(boost::system::error_code {},
                                   "db: scalar column in multi-row VALUES must be an array");
            }
        }
        auto value_at = [&](param const* p, size_t i) -> param const&
        {
            if (auto* arr = std::get_if<param_array>(p))
            {
                return arr->values[i];
            }
            return *p;
        };
        for (size_t i = 0; i < rows; ++i)
        {
            if (i > 0)
            {
                out += "),(";
            }
            for (size_t j = 0; j < g.pending.size(); ++j)
            {
                if (j > 0)
                {
                    out += ',';
                }
                out += b.placeholder(ph++, g.pending_names[j]);
                params.push_back(value_at(g.pending[j], i));
            }
        }
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
     * 数组参数（param 为 param_array）会被展开为多个占位符：`IN (:ids)` 配 `[1,2,3]`
     * 渲染为 `IN (?,?,?)`；`VALUES (:a, :b)` 配等长数组则按列展开为多行
     * `VALUES (?,?),(?,?)`（批量插入，每个占位符表示一列的所有数据，见 \ref expand_values_group）。
     * 空数组抛异常（无法表达空列表）。
     * \n
     * 每个占位符（含同名重复出现）都独立编号并收集一次值，后端决定编号如何映射为文本。
     */
    inline rendered_query
    render_query(std::string_view sql, std::vector<binder> const& binders, backend const& b)
    {
        binder_index idx = build_binder_index(binders);
        if (idx.has_named && idx.has_pos)
        {
            throw std::runtime_error("db: cannot mix positional and named parameters");
        }

        std::string out;
        out.reserve(sql.size() + 16);
        std::vector<param> params;
        bool expanded = false;
        size_t ph = 0; ///< 下一个占位符编号（0 起）

        // 括号分组栈：is_values 组在闭合时按列展开为多行。
        std::vector<values_group_ctx> groups;
        std::string last_word; ///< 最近一个标识符（小写），用于识别 `VALUES` 关键字。

        size_t pos_idx = 0;
        for (size_t i = 0; i < sql.size(); ++i)
        {
            char c = sql[i];
            if (c == '\'' || c == '"' || c == '`')
            {
                if (!groups.empty() && groups.back().is_values && groups.back().has_array)
                {
                    throw std::runtime_error("db: cannot mix literal with array column in VALUES");
                }
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
                    if (!groups.empty() && groups.back().is_values)
                    {
                        auto& g = groups.back();
                        if (std::holds_alternative<param_array>(*p))
                        {
                            // 数组列触发多行展开；组内已原样输出的内容无法对齐多行，拒绝混用。
                            if (g.emitted)
                            {
                                throw std::runtime_error("db: cannot mix literal/scalar before array column in VALUES");
                            }
                            g.pending.push_back(p);
                            g.pending_names.push_back(std::move(name));
                            g.has_array = true;
                        }
                        else if (g.has_array)
                        {
                            // 数组后的标量列也参与展开（行数校验由 expand 完成）。
                            g.pending.push_back(p);
                            g.pending_names.push_back(std::move(name));
                        }
                        else
                        {
                            // 无数组：单行 VALUES，标量占位符原地展开，保留组内字面量。
                            emit_placeholder(out, params, ph, b, p, name);
                            g.emitted = true;
                        }
                    }
                    else
                    {
                        expanded = expanded || emit_placeholder(out, params, ph, b, p, name);
                    }
                    i = start - 1;
                    continue;
                }
            }
            if (c == '(')
            {
                if (!groups.empty() && groups.back().is_values && groups.back().has_array)
                {
                    throw std::runtime_error("db: cannot mix literal with array column in VALUES");
                }
                groups.push_back(values_group_ctx { last_word == "values" || last_word == "value" });
                last_word.clear();
                out += c;
                continue;
            }
            if (c == ')')
            {
                if (!groups.empty() && groups.back().is_values)
                {
                    if (groups.back().has_array)
                    {
                        // 按列展开为多行，行间用 `),(` 分隔，最后一行的 `)` 由本组闭合补上。
                        expand_values_group(out, params, ph, b, groups.back());
                        expanded = expanded || groups.back().has_array;
                    }
                    // 无数组：组内容已原样保留，闭合括号直接补上。
                    out += ')';
                }
                else
                {
                    out += c;
                }
                if (!groups.empty())
                {
                    groups.pop_back();
                }
                last_word.clear();
                continue;
            }
            if (std::isalpha(static_cast<unsigned char>(c)))
            {
                last_word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            else if (!std::isspace(static_cast<unsigned char>(c)))
            {
                last_word.clear();
            }
            // VALUES 数组组的结构字符（逗号/空白）由展开统一重建，这里跳过；
            // 其余字面量（数字/标识符等）无法与按行展开对齐，拒绝混用，禁止静默丢弃。
            if (groups.empty() || !groups.back().is_values || !groups.back().has_array)
            {
                out += c;
                if (!groups.empty() && groups.back().is_values)
                {
                    groups.back().emitted = true;
                }
            }
            else if (c != ',' && !std::isspace(static_cast<unsigned char>(c)))
            {
                throw std::runtime_error("db: cannot mix literal with array column in VALUES");
            }
        }
        if (!idx.has_named && pos_idx != idx.pos.size())
        {
            // 位置参数多于占位符：与后端 prepare 的 arity 检查一致，视为数据库层错误。
            throw db_exception(boost::system::error_code {}, "db: too many parameters for placeholders");
        }
        return { std::move(out), std::move(params), expanded };
    }

} // namespace httplib::db::detail
