#pragma once
#include "beast_alias.hpp"
#include "enum_conv.hpp"
#include "httplib/headers.hpp"
#include <boost/beast/http/fields.hpp>
#include <cstddef>
#include <stdexcept>
#include <string_view>

namespace httplib
{

    /**
     * \brief \ref header 的实现，只存一个 \c basic_fields::const_iterator 。
     * \details
     * 迭代器是 \c basic_fields 的 public typedef，直接用即可；不可命名的只是它指向的
     * 元素类型（\c element_list 是私有 typedef），而 \c it_->name() 这些访问器本就
     * 是 public 的。
     */
    class header::impl
    {
      public:
        /// \brief 库内造 \ref header 的唯一入口。
        /// \details \c impl 是 \c header 的嵌套类，能访问它私有的 \c header() ，故
        /// 无需另开 friend。返回的 \c header 尚未绑定字段，调用方必须紧接着
        /// \c get_impl(h).rebind(...) 。
        static header
        make() noexcept
        {
            return header();
        }

        /// \note 未知字段与枚举外的值都退到 \c field::unknown，名字以
        /// \ref name_string 为准。这里按线上字段名反查本库的字面值表，
        /// 不依赖 Beast 的 \c http::field 集合。
        field
        name() const noexcept
        {
            return enum_conv::field_from_name(name_string());
        }

        std::string_view
        name_string() const noexcept
        {
            return std::string_view(it_->name_string().data(), it_->name_string().size());
        }

        std::string_view
        value() const noexcept
        {
            return std::string_view(it_->value().data(), it_->value().size());
        }

        /// \brief 改指到另一个字段，供 \ref headers::fields 在协程帧里复用同一个
        /// \ref header 沿链走。
        void
        rebind(http::fields::const_iterator it) noexcept
        {
            it_ = it;
        }

      private:
        http::fields::const_iterator it_;
    };

    // 私有嵌套类没法写成类型别名，故用空派生类充当 impl，向上转型零开销。
    class headers::impl : public http::fields
    {
      public:
        /// \note 不能叫 \c count() ：\c http::fields 已有 \c count(name) 重载，加
        /// 无参 \c count() 会遮蔽掉它们，\c headers::count(field) 就编不过了。
        std::size_t
        field_count() const noexcept
        {
            return static_cast<std::size_t>(std::distance(this->begin(), this->end()));
        }

        /// \brief 库内造「借用」视图的唯一入口。\c impl 是 \c headers 的嵌套类，能
        /// 访问它私有的 \c borrow() ，故无需另开 friend。
        static headers
        borrow(http::fields& f) noexcept
        {
            return headers::borrow(static_cast<headers::impl&>(f));
        }
    };

} // namespace httplib
