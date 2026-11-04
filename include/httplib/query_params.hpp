#pragma once
#include "httplib/config.hpp"
#include "httplib/util/misc.hpp"
#include <charconv>
#include <concepts>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace httplib
{
    class HTTPLIB_API query_params
    {
      public:
        /// \brief 有序 multimap：同 key 的多值按插入顺序（\c equal_range 遍历顺序稳定，跨平台一致）。
        using container_type = std::multimap<std::string, std::string>;

        /// \brief 取 key 对应的第一个值。
        /// \tparam T 返回类型，默认 \c std::string_view 。
        /// \param key 键。
        /// \return 转换后的第一个值。
        /// \throws std::runtime_error key 不存在，或值无法转换为 \c T 时。
        /// \note 返回的 \c std::string_view 指向内部存储，对象被修改后失效。
        template <typename T = std::string_view>
            requires std::integral<T> || std::floating_point<T> || std::same_as<T, bool> || std::same_as<T, std::string>
                     || std::same_as<T, std::string_view>
        T
        at(std::string const& key) const
        {
            return convert<T>(at_raw(key));
        }

        /// \brief 同 at(key) ，但 key 不存在时返回 \p fallback 而不抛异常。
        /// \tparam T 返回类型，默认 \c std::string 。
        /// \param key 键。
        /// \param fallback key 不存在时的返回值。
        /// \return key 存在时的转换值，否则返回 \p fallback 。
        /// \throws std::runtime_error key 存在但值无法转换为 \c T 时。
        /// \note 非字符串类型必须显式指定 \c T （如 \c at<int64_t>("k", 0) ）。
        template <typename T = std::string>
            requires std::integral<T> || std::floating_point<T> || std::same_as<T, bool> || std::same_as<T, std::string>
        T
        at(std::string const& key, std::type_identity_t<T> const& fallback) const
        {
            auto iter = params_.find(key);
            if (iter == params_.end())
            {
                return fallback;
            }
            return convert<T>(iter->second);
        }

        /// \brief 取 key 对应的全部值，按插入顺序。
        /// \tparam T 返回类型，默认 \c std::string_view 。
        /// \param key 键。
        /// \return 转换后的值列表；key 不存在时为空列表。
        /// \throws std::runtime_error 任一值无法转换为 \c T 时。
        /// \note 返回的视图指向内部存储，对象被修改后失效。
        template <typename T = std::string_view>
            requires std::integral<T> || std::floating_point<T> || std::same_as<T, bool> || std::same_as<T, std::string>
                     || std::same_as<T, std::string_view>
        std::vector<T>
        all(std::string const& key) const
        {
            std::vector<T> result {};
            for (auto const& sv : all_raw(key))
            {
                result.push_back(convert<T>(sv));
            }
            return result;
        }

        /// \brief 追加一对 key/value，不影响同名 key 的已有值。
        /// \param key 键。
        /// \param val 值；\c bool 存为 \c "true" / \c "false" ，整数与浮点数走 \c std::to_string ，
        ///        其余可转换为 \c std::string_view 的类型按原文存入。
        template <typename T>
            requires std::integral<T> || std::floating_point<T> || std::same_as<T, bool>
                     || std::convertible_to<T, std::string_view>
        void
        add(std::string const& key, T const& val)
        {
            if constexpr (std::same_as<T, bool>)
            {
                add_raw(key, val ? "true" : "false");
            }
            else if constexpr (std::same_as<T, std::string>)
            {
                add_raw(key, std::move(val));
            }
            else if constexpr (std::convertible_to<T, std::string_view>)
            {
                add_raw(key, std::string(std::string_view(val)));
            }
            else
            {
                add_raw(key, std::to_string(val));
            }
        }

        /// \brief 是否存在该 key。
        /// \param key 键。
        /// \return 存在返回 \c true ，否则 \c false 。
        bool has(std::string const& key) const;

        /// \brief 是否没有任何参数。
        /// \return 无参数时返回 \c true 。
        bool empty() const;

        /// \brief 底层有序 multimap，同 key 的多值保持插入顺序。
        /// \return 内部存储的常量引用。
        container_type const& params() const;

        /// \brief 解析 \c application/x-www-form-urlencoded 内容，覆盖现有参数。
        /// \param content 形如 \c a=1&b=2 的已编码文本；空串表示清空。
        /// \return 解析是否成功（当前实现恒为 \c true ）。
        bool decode(std::string_view content);

        /// \brief 编码为 \c application/x-www-form-urlencoded 文本。
        /// \return 形如 \c a=1&b=2 的字符串；同名 key 按插入顺序保留。
        std::string encoded() const;

      private:
        /// \brief 追加原始 key/value，不做类型转换。
        void add_raw(std::string const& key, std::string const& val);

        /// \brief 取 key 的第一个原始值。
        /// \throws std::runtime_error key 不存在时。
        std::string_view at_raw(std::string const& key) const;

        /// \brief 取 key 的全部原始值，按插入顺序。
        std::vector<std::string_view> all_raw(std::string const& key) const;

        /// \brief 把原始字符串转换为 \c T 。
        /// \throws std::runtime_error 转换失败时。
        template <typename T>
            requires std::integral<T> || std::floating_point<T> || std::same_as<T, bool> || std::same_as<T, std::string>
                     || std::same_as<T, std::string_view>
        static T
        convert(std::string_view sv)
        {
            if constexpr (std::same_as<T, std::string_view>)
            {
                return sv;
            }
            else if constexpr (std::same_as<T, std::string>)
            {
                return std::string(sv);
            }
            else if constexpr (std::same_as<T, bool>)
            {
                if (sv == "1" || sv == "true")
                {
                    return true;
                }
                if (sv == "0" || sv == "false")
                {
                    return false;
                }
                throw std::runtime_error("query_param: invalid bool: " + std::string(sv));
            }
            else
            {
                return util::from_chars_strict<T>(sv);
            }
        }

        /// \brief 底层存储，同 key 的多值按插入顺序。
        container_type params_;
    };
} // namespace httplib