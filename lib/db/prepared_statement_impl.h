#pragma once

#include "httplib/db/prepared_statement.hpp"
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace httplib::db
{

    struct prepared_statement::impl
    {
        using param = std::variant<std::monostate,
                                   int64_t,
                                   uint64_t,
                                   double,
                                   std::string,
                                   std::span<std::byte const>,
                                   date,
                                   datetime,
                                   time,
                                   std::chrono::system_clock::time_point>;

        session* session = nullptr;
        std::string sql;           ///< 重写后的 `?` SQL
        std::string original_sql;  ///< 原始 `:name` SQL（日志用）
        std::vector<param> params; ///< 位置绑定值
        std::vector<std::string> param_names;
        std::unordered_map<std::string, param> named_values;
        bool need_params_reset = false;
        bool has_named_bind = false;
        bool has_positional_bind = false;

        void
        begin_bind()
        {
            if (has_named_bind)
            {
                throw std::runtime_error("db: cannot mix positional and named parameters");
            }
            has_positional_bind = true;
            if (need_params_reset)
            {
                params.clear();
                need_params_reset = false;
            }
        }

        void
        begin_named(std::string_view name)
        {
            if (has_positional_bind)
            {
                throw std::runtime_error("db: cannot mix positional and named parameters");
            }
            if (std::find(param_names.begin(), param_names.end(), name) == param_names.end())
            {
                throw std::runtime_error("db: no such named parameter '" + std::string(name) + "'");
            }
            has_named_bind = true;
        }
    };

} // namespace httplib::db
