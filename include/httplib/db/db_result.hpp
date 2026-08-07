#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace httplib::db
{

    struct db_result
    {
        std::vector<std::string> columns;
        std::vector<std::vector<std::string>> rows;

        uint64_t affected_rows = 0;
        uint64_t insert_id = 0;

        [[nodiscard]] bool
        empty() const
        {
            return rows.empty();
        }

        [[nodiscard]] size_t
        size() const
        {
            return rows.size();
        }

        const std::vector<std::string>&
        operator[](size_t index) const
        {
            return rows[index];
        }

        std::vector<std::string>&
        operator[](size_t index)
        {
            return rows[index];
        }

        static constexpr size_t npos = static_cast<size_t>(-1);

        [[nodiscard]] size_t
        column_index(std::string_view name) const
        {
            for (size_t i = 0; i < columns.size(); ++i)
            {
                if (columns[i] == name)
                {
                    return i;
                }
            }
            return npos;
        }
    };

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
