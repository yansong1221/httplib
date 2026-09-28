#pragma once
#include "beast_alias.hpp"
#include "httplib/headers.hpp"
#include <boost/beast/http/fields.hpp>
#include <cstddef>
#include <stdexcept>

namespace httplib
{

    // 私有嵌套类没法写成类型别名，故用空派生类充当 impl，向上转型零开销。
    class headers::impl : public http::fields
    {
      public:
        // 不能叫 count()：http::fields 已有 count(name) 重载，加无参 count() 会
        // 遮蔽掉它们，headers::count(field) 就编不过了。
        std::size_t
        field_count() const noexcept
        {
            return static_cast<std::size_t>(std::distance(this->begin(), this->end()));
        }

        // index 必须是 [0, count())，否则是未定义行为（同所有容器的 operator[]）。
        header
        nth(std::size_t index) const
        {
            auto it = this->begin();
            std::advance(it, static_cast<std::iterator_traits<http::fields::const_iterator>::difference_type>(index));
            auto const& f = *it;
            return header { static_cast<field>(f.name()),
                            std::string_view(f.name_string().data(), f.name_string().size()),
                            std::string_view(f.value().data(), f.value().size()) };
        }
    };

    namespace detail
    {
        // 唯一能在库外看到 headers 内部存储的入口。
        class headers_access
        {
          public:
            static headers
            borrow(http::fields& f) noexcept
            {
                return headers::borrow(static_cast<headers::impl&>(f));
            }

            // 返回 impl& 而非 http::fields&，这样 impl 上加的东西在库内也直接可用。
            static headers::impl&
            raw(headers& h) noexcept
            {
                return *h.p_;
            }

            static headers::impl const&
            raw(headers const& h) noexcept
            {
                return *h.p_;
            }
        };
    } // namespace detail

} // namespace httplib
