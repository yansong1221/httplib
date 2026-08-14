#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/mysql/result.hpp"
#include <boost/mysql.hpp>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace httplib::mysql
{

    struct result::impl
    {
        boost::mysql::results data;
        size_t resultset_index = 0;

        std::vector<std::string> col_names;
        std::vector<mysql::column_type> col_types;
        std::unordered_map<std::string, size_t> col_index;
        uint64_t affected = 0;
        uint64_t insert_id = 0;
        uint64_t warnings = 0;
        std::chrono::seconds utc_offset { 0 };

        impl() = default;
        explicit impl(boost::mysql::results&& r, std::chrono::seconds offset = {});

        void load_resultset(size_t idx);
        boost::mysql::rows_view rows() const;
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
