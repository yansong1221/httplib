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

    /**
     * \brief 结果集列的类型。
     */
    enum class column_type
    {
        string,    ///< 字符串（CHAR/VARCHAR/TEXT/JSON 等）。
        int64,     ///< 有符号整数。
        uint64,    ///< 无符号整数。
        double_,   ///< 浮点数。
        blob,      ///< 二进制数据。
        date,      ///< DATE。
        datetime,  ///< DATETIME。
        timestamp, ///< TIMESTAMP（时区敏感）。
        time,      ///< TIME。
        null,      ///< NULL。
        unknown    ///< 未知类型。
    };

    /**
     * \brief 查询结果集。
     * \details
     * 由 \ref session::query 或 \ref prepared_statement::execute 返回。
     * \n
     * 多语句查询会包含多个结果集，可通过 \ref resultset_count / \ref next_resultset 遍历。
     * \n
     * 支持范围 for 遍历行（通过 \ref begin / \ref end）。
     */
    class HTTPLIB_API result
    {
      public:
        result();
        result(result&&) noexcept;
        result& operator=(result&&) noexcept;
        ~result();

        result(result const&) = delete;
        result& operator=(result const&) = delete;

        /**
         * \brief 当前结果集是否没有行。
         */
        bool empty() const;

        /**
         * \brief 结果集数量（多语句查询时 > 1）。
         */
        size_t resultset_count() const;

        /**
         * \brief 前进到下一个结果集，返回是否成功。
         */
        bool next_resultset();

        /**
         * \brief 当前结果集的行数。
         */
        size_t row_count() const;

        /**
         * \brief 影响的行数（INSERT/UPDATE/DELETE 等）。
         */
        uint64_t affected_rows() const;

        /**
         * \brief 最后插入的自增 ID。
         */
        uint64_t last_insert_id() const;

        /**
         * \brief 警告数量。
         */
        uint64_t warning_count() const;

        /**
         * \brief 列数。
         */
        size_t column_count() const;

        /**
         * \brief 按列名查找列下标，找不到抛异常。
         */
        size_t column_index(std::string_view name) const;

        /**
         * \brief 按列下标取列名。
         */
        std::string const& column_name(size_t col) const;

        /**
         * \brief 按列下标取列类型。
         */
        mysql::column_type column_type(size_t col) const;

        /**
         * \brief 按行下标取行（不进行越界检查）。
         */
        row operator[](size_t index) const;

        /**
         * \brief 行迭代器（input iterator）。
         */
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

        /**
         * \brief 返回指向第一行的迭代器。
         */
        iterator begin() const;

        /**
         * \brief 返回指向末尾的迭代器。
         */
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
