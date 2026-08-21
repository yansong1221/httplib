#pragma once

#include "httplib/config.hpp"
#include "temporal.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace httplib::db
{

    /**
     * \brief 结果集列的类型（后端无关）。
     * \details
     * SQLite 只有 NULL/INTEGER/REAL/TEXT/BLOB 五种存储类型，date/datetime/time 均以 TEXT 存储，
     * 通过列声明的类型名（DATE/DATETIME/TIME）区分；MySQL 有原生类型。
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
        timestamp, ///< TIMESTAMP（时区敏感，MySQL 专有）。
        time,      ///< TIME。
        null,      ///< NULL。
        unknown    ///< 未知类型。
    };

    /**
     * \brief 拥有型或借用型的二进制数据。
     * \details
     * - 拥有型：内部持有 vector，data() 指向其存储；
     * - 借用型：data() 指向外部 buffer（如 MySQL results 内部缓冲），owner 保活；
     *   构造时未传锚点则视为调用方自行保证其生命周期。
     * \n
     * 无论哪种形态，blob 对象存活期内 data() 均有效。
     */
    class blob
    {
      public:
        /// 拥有型：接管 vector。
        blob(std::vector<std::byte> v)
        {
            auto p = std::make_shared<std::vector<std::byte>>(std::move(v));
            data_ = std::span<std::byte const>(p->data(), p->size());
            owner_ = std::move(p);
        }

        /// 借用型：外部 buffer + 保活锚点（可为空）。
        blob(std::span<std::byte const> view, std::shared_ptr<void> anchor = {})
            : data_(view), owner_(std::move(anchor))
        {
        }

        std::span<std::byte const> data() const noexcept { return data_; }
        size_t size() const noexcept { return data_.size(); }
        bool empty() const noexcept { return data_.empty(); }
        bool owned() const noexcept { return owner_ != nullptr; }

      private:
        std::span<std::byte const> data_;
        std::shared_ptr<void> owner_;
    };

    /**
     * \brief 拥有型或借用型的文本数据。
     * \details
     * - 拥有型：内部持有 string，data() 指向其存储；
     * - 借用型：data() 指向外部 buffer（如 MySQL results 内部缓冲），owner 保活；
     *   构造时未传锚点则视为调用方自行保证其生命周期。
     * \n
     * 无论哪种形态，text 对象存活期内 data() 均有效。
     */
    class text
    {
      public:
        /// 拥有型：接管 string。
        text(std::string s)
        {
            auto p = std::make_shared<std::string>(std::move(s));
            data_ = std::string_view(p->data(), p->size());
            owner_ = std::move(p);
        }

        /// 借用型：外部 buffer + 保活锚点（可为空）。
        text(std::string_view view, std::shared_ptr<void> anchor = {})
            : data_(view), owner_(std::move(anchor))
        {
        }

        std::string_view data() const noexcept { return data_; }
        size_t size() const noexcept { return data_.size(); }
        bool empty() const noexcept { return data_.empty(); }
        bool owned() const noexcept { return owner_ != nullptr; }

      private:
        std::string_view data_;
        std::shared_ptr<void> owner_;
    };

    /**
     * \brief 一个字段值。
     * \details
     * 标量/日期类型为拥有型；文本为 \ref text、二进制为 \ref blob（各自拥有或借用）；
     * MySQL 后端的 blob/文本可借用其结果集 buffer，借用字段随 \ref result 存活（由内部锚点保活）。
     * \n
     * - \c std::monostate 表示 NULL
     * - 整数/浮点/字符串/二进制为值
     * - date/datetime/time 为无时区墙上时钟
     * - \c timestamp 为 UTC 时间点（TIMESTAMP 等时区敏感列专用，与 datetime 不混用）
     */
    using field = std::variant<std::monostate,
                               int64_t,
                               uint64_t,
                               double,
                               text,
                               blob,
                               date,
                               datetime,
                               time,
                               timestamp>;

} // namespace httplib::db
