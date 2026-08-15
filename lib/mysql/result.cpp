#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/mysql/result.hpp"
#include "mysql/result_impl.h"

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

    void
    result::impl::load_resultset(size_t idx)
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

    result::impl::impl(boost::mysql::results&& r, std::chrono::seconds offset) : data(std::move(r)), utc_offset(offset)
    {
        if (data.has_value())
        {
            load_resultset(0);
        }
    }

    boost::mysql::rows_view
    result::impl::rows() const
    {
        return data[resultset_index].rows();
    }

    result::result() : impl_(std::make_unique<impl>()) {}
    result::result(std::unique_ptr<impl> p) : impl_(std::move(p)) {}
    result::result(result&&) noexcept = default;
    result& result::operator=(result&&) noexcept = default;
    result::~result() = default;

    result::impl&
    get_impl(result& s)
    {
        return *s.impl_;
    }
    result::impl const&
    get_impl(result const& s)
    {
        return *s.impl_;
    }

    bool
    result::empty() const
    {
        auto& i = get_impl(*this);
        return !i.data.has_value() || i.rows().empty();
    }

    size_t
    result::resultset_count() const
    {
        auto& i = get_impl(*this);
        return i.data.has_value() ? i.data.size() : 0;
    }

    bool
    result::next_resultset()
    {
        auto& i = get_impl(*this);
        if (!i.data.has_value() || i.resultset_index + 1 >= i.data.size())
        {
            return false;
        }
        ++i.resultset_index;
        i.load_resultset(i.resultset_index);
        return true;
    }

    size_t
    result::row_count() const
    {
        auto& i = get_impl(*this);
        return i.data.has_value() ? i.rows().size() : 0;
    }

    uint64_t
    result::affected_rows() const
    {
        return get_impl(*this).affected;
    }

    uint64_t
    result::last_insert_id() const
    {
        return get_impl(*this).insert_id;
    }

    uint64_t
    result::warning_count() const
    {
        return get_impl(*this).warnings;
    }

    size_t
    result::column_count() const
    {
        return get_impl(*this).col_names.size();
    }

    size_t
    result::column_index(std::string_view n) const
    {
        auto& i = get_impl(*this);
        auto it = i.col_index.find(n);
        if (it == i.col_index.end())
        {
            throw std::runtime_error("db: column not found: " + std::string(n));
        }
        return it->second;
    }

    std::string const&
    result::column_name(size_t c) const
    {
        return get_impl(*this).col_names.at(c);
    }

    column_type
    result::column_type(size_t c) const
    {
        return get_impl(*this).col_types.at(c);
    }

    row
    result::operator[](size_t i) const
    {
        return row(this, i);
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

} // namespace httplib::mysql
#endif
