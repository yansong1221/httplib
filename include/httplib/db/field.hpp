#pragma once

#include "httplib/config.hpp"
#include "temporal.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
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
     * \brief 一个拥有型的字段值。
     * \details
     * 后端负责把原生值深拷贝进本类型，不持有任何后端内部 buffer 的引用。
     * \n
     * - \c std::monostate 表示 NULL
     * - 整数/浮点/字符串/二进制为值
     * - date/datetime/time 为时间值
     */
    using field = std::
        variant<std::monostate, int64_t, uint64_t, double, std::string, std::vector<std::byte>, date, datetime, time>;

} // namespace httplib::db
