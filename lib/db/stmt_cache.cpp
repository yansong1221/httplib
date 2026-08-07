#ifdef HTTPLIB_ENABLED_DATABASE
#include "db/stmt_cache.h"

namespace httplib::db
{

    stmt_cache::stmt_cache(size_t max_size)
        : max_size_(max_size)
    {
    }

    boost::mysql::statement*
    stmt_cache::find(std::string_view sql)
    {
        if (max_size_ == 0)
        {
            return nullptr;
        }

        if (auto it = map_.find(sql); it == map_.end())
        {
            return nullptr;
        }
        else
        {
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
            return &(it->second->second);
        }
    }

    std::optional<boost::mysql::statement>
    stmt_cache::insert(const std::string& sql, boost::mysql::statement stmt)
    {
        if (max_size_ == 0)
        {
            return stmt;
        }

        std::optional<boost::mysql::statement> evicted;

        if (auto it = map_.find(sql); it != map_.end())
        {
            it->second->second = std::move(stmt);
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
            return evicted;
        }

        if (lru_list_.size() >= max_size_)
        {
            auto& back = lru_list_.back();
            evicted = std::move(back.second);
            map_.erase(back.first);
            lru_list_.pop_back();
        }

        lru_list_.emplace_front(sql, std::move(stmt));
        map_[sql] = lru_list_.begin();

        return evicted;
    }

    std::optional<boost::mysql::statement>
    stmt_cache::erase(std::string_view sql)
    {
        if (auto it = map_.find(sql); it == map_.end())
        {
            return std::nullopt;
        }
        else
        {
            auto stmt = std::move(it->second->second);
            lru_list_.erase(it->second);
            map_.erase(it);
            return stmt;
        }
    }

    std::vector<boost::mysql::statement>
    stmt_cache::clear()
    {
        std::vector<boost::mysql::statement> stmts;
        stmts.reserve(lru_list_.size());
        for (auto& entry : lru_list_)
        {
            stmts.push_back(std::move(entry.second));
        }
        lru_list_.clear();
        map_.clear();
        return stmts;
    }

    size_t
    stmt_cache::size() const
    {
        return lru_list_.size();
    }

    size_t
    stmt_cache::max_size() const
    {
        return max_size_;
    }

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
