#pragma once

#include "httplib/config.hpp"
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace httplib::db
{

    /**
     * \brief 后端连接选项（backend 无关的连接串 key=value 视图）。
     * \details
     * 连接串是空白分隔的 `key=value` 序列，等价于 SOCI 的 connection string，例如：
     * \n
     *   `"host=127.0.0.1 port=3306 user=root password=123456 db=main time_zone=+08:00"`
     * \n
     * 语法规则：
     * \n
     * - 多个 `key=value` 之间以空白分隔；
     * - 键是非空标识符，不含 `=` 与空白；
     * - 值默认取到下一个空白；需要包含空格、`=` 或引号时，用单引号或双引号包裹，
     *   内部可用反斜杠 `\\` 转义；
     * - 同一键出现多次时，后者覆盖前者；
     * - 空连接串等价于空 options。
     * \n
     * 由 \ref session::connect / \ref make_pool 解析后交给对应后端工厂，
     * 因此新增后端无需扩展统一层 API（只增加后端注册）。
     * 各后端支持的键见 \ref mysql_config 与 \ref sqlite_config。
     */
    class HTTPLIB_API options
    {
      public:
        options() = default;

        /// 解析连接串为选项集合；空串返回空 options。规则见类注释。
        static options parse(std::string_view conn_string);

        /// 设置一个选项（覆盖已存在的同名键）。
        void set(std::string_view key, std::string value);

        /// 是否存在指定键。
        bool has(std::string_view key) const;

        /// 取指定键的值；键不存在返回 std::nullopt。
        std::optional<std::string> get(std::string_view key) const;

        /// 取指定键的值；键不存在返回 \p def。
        std::string get_or(std::string_view key, std::string def = {}) const;

        /// 解析为 uint16（如端口）；键缺失或格式非法返回 std::nullopt。
        std::optional<uint16_t> as_uint16(std::string_view key) const;

        /// 解析为 int；键缺失或格式非法返回 std::nullopt。
        std::optional<int> as_int(std::string_view key) const;

        /// 解析为秒；键缺失或格式非法返回 std::nullopt。
        std::optional<std::chrono::seconds> as_seconds(std::string_view key) const;

        /// 解析为 bool（`1/true/yes/on` 与 `0/false/no/off`）；否则返回 std::nullopt。
        std::optional<bool> as_bool(std::string_view key) const;

        /// 序列化回连接串（供配置结构转回字符串 API 使用），与 \ref parse 互为逆操作。
        std::string to_connection_string() const;

      private:
        std::map<std::string, std::string, std::less<>> kv_;
    };

} // namespace httplib::db
