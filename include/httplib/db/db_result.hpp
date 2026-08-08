#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include "httplib/db/row.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace httplib::db
{

    class HTTPLIB_API db_result
    {
      public:
        db_result();
        db_result(db_result&&) noexcept;
        db_result& operator=(db_result&&) noexcept;
        ~db_result();

        db_result(db_result const&) = delete;
        db_result& operator=(db_result const&) = delete;

        bool empty() const;

        size_t row_count() const;

        uint64_t affected_rows() const;

        uint64_t last_insert_id() const;

        uint64_t warning_count() const;

        size_t column_count() const;

        size_t column_index(std::string_view name) const;

        std::string const& column_name(size_t col) const;

        column_type column_type(size_t col) const;

        row operator[](size_t index) const;

        class iterator
        {
          public:
            using value_type = row;
            using reference = row;
            using pointer = void;
            using difference_type = std::ptrdiff_t;
            using iterator_category = std::input_iterator_tag;

            row
            operator*() const
            {
                return (*result_)[idx_];
            }
            iterator&
            operator++()
            {
                ++idx_;
                return *this;
            }
            iterator
            operator++(int)
            {
                auto tmp = *this;
                ++idx_;
                return tmp;
            }
            bool
            operator==(iterator const& other) const
            {
                return idx_ == other.idx_;
            }
            bool
            operator!=(iterator const& other) const
            {
                return idx_ != other.idx_;
            }

          private:
            db_result const* result_;
            size_t idx_;
            iterator(db_result const* result, size_t idx) : result_(result), idx_(idx) {}
            friend class db_result;
        };

        iterator begin() const;

        iterator end() const;

        struct impl;

        explicit db_result(std::unique_ptr<impl> p);

      private:
        std::unique_ptr<impl> impl_;

        friend impl& get_impl(db_result& self);
        friend impl const& get_impl(db_result const& self);
    };

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
