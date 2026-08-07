#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include <boost/mysql/statement.hpp>
#include <cstddef>
#include <list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace httplib::db
{

    struct string_hash
    {
        using is_transparent = void;

        size_t
        operator()(std::string_view sv) const
        {
            return std::hash<std::string_view> {}(sv);
        }

        size_t
        operator()(const std::string& s) const
        {
            return std::hash<std::string_view> {}(s);
        }
    };

    struct string_equal
    {
        using is_transparent = void;

        bool
        operator()(std::string_view a, std::string_view b) const
        {
            return a == b;
        }
    };

    class HTTPLIB_API stmt_cache
    {
      public:
        explicit stmt_cache(size_t max_size = 64);

        [[nodiscard]] boost::mysql::statement*
        find(std::string_view sql);
        [[nodiscard]] std::optional<boost::mysql::statement>
        insert(const std::string& sql, boost::mysql::statement stmt);
        [[nodiscard]] std::optional<boost::mysql::statement>
        erase(std::string_view sql);
        [[nodiscard]] std::vector<boost::mysql::statement>
        clear();

        [[nodiscard]] size_t
        size() const;
        [[nodiscard]] size_t
        max_size() const;

      private:
        size_t max_size_;

        using lru_entry = std::pair<std::string, boost::mysql::statement>;
        std::list<lru_entry> lru_list_;

        std::unordered_map<std::string, std::list<lru_entry>::iterator, string_hash, string_equal> map_;
    };

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
