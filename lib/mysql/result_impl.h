#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/mysql/result.hpp"
#include <boost/mysql.hpp>
#include <string>
#include <vector>

namespace httplib::mysql
{

    struct result::impl
    {
        boost::mysql::results data;

        std::vector<std::string> col_names;
        std::vector<mysql::column_type> col_types;

        uint64_t affected = 0;
        uint64_t insert_id = 0;
        uint64_t warnings = 0;
    };

    HTTPLIB_API void build_result_impl(result::impl& imp);

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
