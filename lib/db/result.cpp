#include "httplib/db/result.hpp"
#include "httplib/db/exception.hpp"
#include <stdexcept>

namespace httplib::db
{

    result::result() = default;
    result::result(result&&) noexcept = default;
    result& result::operator=(result&&) noexcept = default;
    result::~result() = default;

    result::result(std::vector<resultset> sets)
        : sets_(std::move(sets))
    {
    }

    result::resultset const&
    result::cur() const
    {
        static resultset const empty_set {};
        return sets_.empty() ? empty_set : sets_.at(idx_);
    }

    field const&
    result::at(size_t row, size_t col) const
    {
        auto const& rs = cur();
        if (row >= rs.rows.size())
        {
            throw std::out_of_range("db: row index out of range");
        }
        if (col >= rs.rows[row].size())
        {
            throw std::out_of_range("db: column index out of range");
        }
        return rs.rows[row][col];
    }

    bool
    result::empty() const
    {
        return sets_.empty() || cur().rows.empty();
    }

    size_t
    result::resultset_count() const
    {
        return sets_.size();
    }

    bool
    result::next_resultset()
    {
        if (idx_ + 1 >= sets_.size())
        {
            return false;
        }
        ++idx_;
        return true;
    }

    size_t
    result::row_count() const
    {
        return cur().rows.size();
    }

    uint64_t
    result::affected_rows() const
    {
        return cur().affected;
    }

    uint64_t
    result::last_insert_id() const
    {
        return cur().last_insert_id;
    }

    size_t
    result::column_count() const
    {
        return cur().names.size();
    }

    size_t
    result::column_index(std::string_view name) const
    {
        auto& names = cur().names;
        for (size_t i = 0; i < names.size(); ++i)
        {
            if (names[i] == name)
            {
                return i;
            }
        }
        throw db_exception(boost::system::error_code {}, "db: column not found: " + std::string(name));
    }

    std::string const&
    result::column_name(size_t col) const
    {
        return cur().names.at(col);
    }

    db::column_type
    result::column_type(size_t col) const
    {
        return cur().types.at(col);
    }

    row
    result::operator[](size_t index) const
    {
        return row(this, index);
    }

    result::iterator
    result::begin() const
    {
        return iterator(this, 0);
    }

    result::iterator
    result::end() const
    {
        return iterator(this, row_count());
    }

} // namespace httplib::db
