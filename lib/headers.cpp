#include "beast_alias.hpp"
#include "enum_conv.hpp"
#include "headers_impl.hpp"
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace httplib
{

    namespace
    {
        // beast::string_view 与 std::string_view 在不同 Boost 配置下可能是同一类型，
        // 也可能是两个类，故双向显式转换，不依赖任何一种假设。
        std::string_view
        to_sv(beast::string_view v) noexcept
        {
            return std::string_view(v.data(), v.size());
        }

        beast::string_view
        to_bsv(std::string_view v) noexcept
        {
            return beast::string_view(v.data(), v.size());
        }

        std::string_view
        first_of(http::fields const& f, http::field name)
        {
            auto it = f.find(name);
            return it == f.end() ? std::string_view {} : to_sv(it->value());
        }

        std::string_view
        first_of(http::fields const& f, std::string_view name)
        {
            auto it = f.find(name);
            return it == f.end() ? std::string_view {} : to_sv(it->value());
        }

        template <typename Key>
        std::vector<std::string_view>
        collect(http::fields const& f, Key name)
        {
            std::vector<std::string_view> out;
            auto const r = f.equal_range(name);
            for (auto it = r.first; it != r.second; ++it)
            {
                out.push_back(to_sv(it->value()));
            }
            return out;
        }
    } // namespace

    // ---- headers::const_iterator ----
    //
    // 迭代器只有「impl 指针 + 序号」两个成员，语义全部落在 impl 上
    // （见 lib/headers_impl.hpp 的 nth/count），所以这里只剩四个转发。

    header
    headers::const_iterator::operator*() const
    {
        return fields_->nth(index_);
    }

    header_accessor
    headers::const_iterator::operator->() const
    {
        return header_accessor(fields_->nth(index_));
    }

    headers::const_iterator&
    headers::const_iterator::operator++()
    {
        ++index_;
        return *this;
    }

    headers::const_iterator
    headers::const_iterator::operator++(int)
    {
        auto copy = *this;
        ++index_;
        return copy;
    }

    // ---- headers ----

    headers::headers(borrowed_tag, impl& borrowed) noexcept : p_(&borrowed), owned_(nullptr) {}

    headers
    headers::borrow(impl& borrowed) noexcept
    {
        return headers(borrowed_tag {}, borrowed);
    }

    headers::headers() : owned_(std::make_unique<impl>()) { p_ = owned_.get(); }

    headers::headers(headers const& other)
    {
        if (other.owned_)
        {
            // 自有 -> 自有：深拷贝。
            owned_ = std::make_unique<impl>(*other.owned_);
            p_ = owned_.get();
        }
        else
        {
            // 借用 -> 借用：仍是同一集合的视图（string_view 语义）。
            p_ = other.p_;
        }
    }

    headers::headers(headers&& other) noexcept : owned_(std::move(other.owned_)), p_(other.p_) { other.p_ = nullptr; }

    headers&
    headers::operator=(headers const& other)
    {
        if (this != &other)
        {
            headers tmp(other);
            *this = std::move(tmp);
        }
        return *this;
    }

    headers&
    headers::operator=(headers&& other) noexcept
    {
        if (this != &other)
        {
            owned_ = std::move(other.owned_);
            p_ = other.p_;
            other.p_ = nullptr;
        }
        return *this;
    }

    headers::~headers() = default;

    void
    headers::clear()
    {
        detail::headers_access::raw(*this).clear();
    }

    std::string_view
    headers::operator[](field name) const
    {
        return first_of(detail::headers_access::raw(*this), enum_conv::to_field(name));
    }

    std::string_view
    headers::operator[](std::string_view name) const
    {
        return first_of(detail::headers_access::raw(*this), name);
    }

    std::string_view
    headers::at(field name) const
    {
        return to_sv(detail::headers_access::raw(*this).at(enum_conv::to_field(name)));
    }

    std::string_view
    headers::at(std::string_view name) const
    {
        return to_sv(detail::headers_access::raw(*this).at(name));
    }

    void
    headers::set(field name, std::string_view value)
    {
        detail::headers_access::raw(*this).set(enum_conv::to_field(name), to_bsv(value));
    }

    void
    headers::set(std::string_view name, std::string_view value)
    {
        detail::headers_access::raw(*this).set(name, to_bsv(value));
    }

    void
    headers::insert(field name, std::string_view value)
    {
        detail::headers_access::raw(*this).insert(enum_conv::to_field(name), to_bsv(value));
    }

    void
    headers::insert(std::string_view name, std::string_view value)
    {
        detail::headers_access::raw(*this).insert(name, to_bsv(value));
    }

    void
    headers::erase(field name)
    {
        detail::headers_access::raw(*this).erase(enum_conv::to_field(name));
    }

    void
    headers::erase(std::string_view name)
    {
        detail::headers_access::raw(*this).erase(name);
    }

    bool
    headers::has(field name) const
    {
        auto const& f = detail::headers_access::raw(*this);
        return f.find(enum_conv::to_field(name)) != f.end();
    }

    bool
    headers::has(std::string_view name) const
    {
        auto const& f = detail::headers_access::raw(*this);
        return f.find(name) != f.end();
    }

    std::size_t
    headers::count(field name) const
    {
        return detail::headers_access::raw(*this).count(enum_conv::to_field(name));
    }

    std::size_t
    headers::count(std::string_view name) const
    {
        return detail::headers_access::raw(*this).count(name);
    }

    std::vector<std::string_view>
    headers::values(field name) const
    {
        return collect(detail::headers_access::raw(*this), enum_conv::to_field(name));
    }

    std::vector<std::string_view>
    headers::values(std::string_view name) const
    {
        return collect(detail::headers_access::raw(*this), name);
    }

    std::vector<header>
    headers::all() const
    {
        std::vector<header> out;
        out.reserve(size());
        for (auto const& h : *this)
        {
            out.push_back(h);
        }
        return out;
    }

    headers::const_iterator
    headers::begin() const noexcept
    {
        auto const& f = detail::headers_access::raw(*this);
        return const_iterator(&f, 0);
    }

    headers::const_iterator
    headers::end() const noexcept
    {
        auto const& f = detail::headers_access::raw(*this);
        return const_iterator(&f, f.field_count());
    }

    headers::const_iterator
    headers::cbegin() const noexcept
    {
        return begin();
    }

    headers::const_iterator
    headers::cend() const noexcept
    {
        return end();
    }

    std::size_t
    headers::size() const
    {
        return detail::headers_access::raw(*this).field_count();
    }

    bool
    headers::empty() const
    {
        return size() == 0;
    }

    void
    headers::merge(headers const& other)
    {
        auto& dst = detail::headers_access::raw(*this);
        auto const& src = detail::headers_access::raw(other);
        for (auto it = src.begin(); it != src.end(); ++it)
        {
            // 必须用 name_string()：非标准头的 name() 是 field::unknown，
            // 传给 insert(field, ...) 会被 beast 拒绝。标准头的 name_string()
            // 就是该枚举的规范拼写，结果与 insert(field, ...) 一致。
            dst.insert(it->name_string(), it->value());
        }
    }

} // namespace httplib
