#pragma once
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace httplib::util
{

    /**
     * \brief 透明 hash，允许以 std::string 为 key 的 unordered_map 直接用 string_view / const char* 做异构查找，
     * 免去每次查找临时构造 std::string 的堆分配。
     */
    struct string_view_hash
    {
        using is_transparent = void;

        size_t
        operator()(std::string_view sv) const noexcept
        {
            return std::hash<std::string_view> {}(sv);
        }

        size_t
        operator()(std::string const& s) const noexcept
        {
            return std::hash<std::string_view> {}(s);
        }

        size_t
        operator()(char const* s) const noexcept
        {
            return std::hash<std::string_view> {}(s);
        }
    };

    /**
     * \brief 以 std::string 为 key、但支持 string_view 异构查找的 unordered_map。
     */
    template <typename T>
    using string_map = std::unordered_map<std::string, T, string_view_hash, std::equal_to<>>;

} // namespace httplib::util
