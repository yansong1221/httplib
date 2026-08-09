#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include "httplib/mysql/row.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace httplib::mysql
{

    class HTTPLIB_API result
    {
      public:
        result();
        result(result&&) noexcept;
        result& operator=(result&&) noexcept;
        ~result();

        result(result const&) = delete;
        result& operator=(result const&) = delete;

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
            result const* result_;
            size_t idx_;
            iterator(result const* result, size_t idx) : result_(result), idx_(idx) {}
            friend class result;
        };

        iterator begin() const;

        iterator end() const;

        struct impl;

        explicit result(std::unique_ptr<impl> p);

      private:
        std::unique_ptr<impl> impl_;

        friend impl& get_impl(result& self);
        friend impl const& get_impl(result const& self);
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
