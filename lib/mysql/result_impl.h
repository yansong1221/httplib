#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/mysql/result.hpp"
#include "httplib/util/string_hash.hpp"
#include <boost/mysql.hpp>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace httplib::mysql
{

    namespace detail
    {

        static column_type
        map_column_type(boost::mysql::column_type t, bool u)
        {
            using b = boost::mysql::column_type;
            switch (t)
            {
                case b::tinyint:
                case b::smallint:
                case b::mediumint:
                case b::int_:
                case b::bigint:
                case b::year:
                    return u ? column_type::uint64 : column_type::int64;
                case b::bit:
                    return column_type::uint64;
                case b::float_:
                case b::double_:
                case b::decimal:
                    return column_type::double_;
                case b::varchar:
                case b::char_:
                case b::text:
                case b::enum_:
                case b::set:
                case b::json:
                    return column_type::string;
                case b::blob:
                case b::geometry:
                    return column_type::blob;
                case b::date:
                    return column_type::date;
                case b::datetime:
                    return column_type::datetime;
                case b::timestamp:
                    return column_type::timestamp;
                case b::time:
                    return column_type::time;
                default:
                    return column_type::unknown;
            }
        }

    } // namespace detail

    struct result::impl
    {
        boost::mysql::results data;
        size_t resultset_index = 0;

        std::vector<std::string> col_names;
        std::vector<mysql::column_type> col_types;
        util::string_map<size_t> col_index;
        uint64_t affected = 0;
        uint64_t insert_id = 0;
        uint64_t warnings = 0;
        std::chrono::seconds utc_offset { 0 };

        impl() = default;
        explicit impl(boost::mysql::results&& r, std::chrono::seconds offset = {})
            : data(std::move(r))
            , utc_offset(offset)
        {
            if (data.has_value())
            {
                load_resultset(0);
            }
        }

        void
        load_resultset(size_t idx)
        {
            auto rs = data[idx];
            affected = rs.affected_rows();
            insert_id = rs.last_insert_id();
            warnings = rs.warning_count();
            col_names.clear();
            col_types.clear();
            col_index.clear();
            if (rs.has_value())
            {
                auto m = rs.meta();
                col_names.reserve(m.size());
                col_types.reserve(m.size());
                col_index.reserve(m.size());
                for (auto& c : m)
                {
                    auto s = c.column_name();
                    col_names.emplace_back(s.data(), s.size());
                    col_types.push_back(detail::map_column_type(c.type(), c.is_unsigned()));
                    col_index.emplace(col_names.back(), col_names.size() - 1);
                }
            }
        }
        boost::mysql::rows_view
        rows() const
        {
            return data[resultset_index].rows();
        }
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
