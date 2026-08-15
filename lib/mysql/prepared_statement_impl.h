#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/mysql/prepared_statement.hpp"
#include <boost/mysql/field_view.hpp>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace httplib::mysql
{

    struct prepared_statement::impl
    {
        /// 参数值：field_view 为标量/非拥有视图（int/double/date/blob 等）；std::string 为拥有型字符串（普通字符串与
        /// JSON 序列化结果）
        using param = std::variant<boost::mysql::field_view, std::string>;

        session* session = nullptr;
        std::string sql;
        std::string original_sql;
        std::vector<param> params;

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
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
