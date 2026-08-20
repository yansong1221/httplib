#pragma once

#include "field.hpp"
#include "fwd.hpp"
#include "httplib/config.hpp"
#include "row.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace httplib::db
{

    /**
     * \brief 查询结果集。
     * \details
     * 由 \ref session::query 或 \ref prepared_statement::execute 返回。
     * \n
     * 所有行数据均为拥有型（存于本对象内部），不持有后端 buffer 的引用。
     * \n
     * 多语句查询（MySQL multi_queries）会包含多个结果集，可通过 \ref resultset_count /
     * \ref next_resultset 遍历。
     * \n
     * 支持范围 for 遍历当前结果集的行。
     */
    class HTTPLIB_API result
    {
      public:
        /// 单个结果集的数据（后端填充用）。
        struct resultset
        {
            std::vector<std::vector<field>> rows;
            std::vector<std::string> names;
            std::vector<db::column_type> types;
            uint64_t affected = 0;
            uint64_t last_insert_id = 0;
        };

        result();
        result(result&&) noexcept;
        result& operator=(result&&) noexcept;
        ~result();

        result(result const&) = delete;
        result& operator=(result const&) = delete;

        /// 结果集是否没有行。
        bool empty() const;

        /// 结果集数量（多语句查询时 > 1）。
        size_t resultset_count() const;

        /// 前进到下一个结果集，返回是否成功。
        bool next_resultset();

        /// 当前结果集的行数。
        size_t row_count() const;

        /// 影响的行数（INSERT/UPDATE/DELETE 等）。
        uint64_t affected_rows() const;

        /// 最后插入的自增 ID。
        uint64_t last_insert_id() const;

        /// 列数。
        size_t column_count() const;

        /// 按列名查找列下标，找不到抛异常。
        size_t column_index(std::string_view name) const;

        /// 按列下标取列名。
        std::string const& column_name(size_t col) const;

        /// 按列下标取列类型。
        db::column_type column_type(size_t col) const;

        /// 按行下标取行（不进行越界检查）。
        row operator[](size_t index) const;

        /// 行迭代器（input iterator）。
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

        /// 由后端填充的构造入口。
        explicit result(std::vector<resultset> sets);

      private:
        resultset const& cur() const;
        field const& at(size_t row, size_t col) const;

        std::vector<resultset> sets_;
        size_t idx_ = 0;

        friend class row;
    };

} // namespace httplib::db
