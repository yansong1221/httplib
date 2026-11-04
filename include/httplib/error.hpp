#pragma once
#include "httplib/config.hpp"
#include <boost/system/error_code.hpp>
#include <type_traits>

namespace httplib
{
    /// \brief 通用错误码。
    enum class error
    {
        /// 请求的项不存在。
        not_found = 1,
        /// 值存在但无法转换为请求的类型。
        invalid_value,
    };

    /// \brief 构造 \c httplib 类别的 error_code。
    HTTPLIB_API boost::system::error_code make_error_code(error e);

} // namespace httplib

namespace boost::system
{
    /// \brief 让 httplib::error 可直接隐式转换为 error_code。
    template <>
    struct is_error_code_enum<httplib::error> : std::true_type
    {
    };
} // namespace boost::system
