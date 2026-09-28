#include "beast_alias.hpp"
#include "enum_conv.hpp"
#include "headers_impl.hpp"
#include <iterator>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

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
        // 同一个名字的全部值，顺序即线上顺序。http::field 查表，string_view 走
        // beast 的大小写不敏感查找，故按 Key 分派。
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

    // ---- header ----

    // 只能由 headers::fields 产出，拷贝一次即一次堆分配：遍历时务必用 auto const&，
    // 让协程帧里那一个复用。
    header::header() noexcept : impl_(std::make_unique<impl>()) {}

    header::header(header const& other) : impl_(std::make_unique<impl>(*other.impl_)) {}

    // 移动会掏空 impl，被移走的 header 随即不可用（同 moved-from 容器约定）。
    header::header(header&& other) noexcept : impl_(std::move(other.impl_)) {}

    header&
    header::operator=(header const& other)
    {
        if (this != &other)
        {
            impl_ = std::make_unique<impl>(*other.impl_);
        }
        return *this;
    }

    header&
    header::operator=(header&& other) noexcept
    {
        if (this != &other)
        {
            impl_ = std::move(other.impl_);
        }
        return *this;
    }

    // 唯一定义在 lib/ 的原因：unique_ptr<impl> 的析构要求 impl 完整。
    header::~header() = default;

    field
    header::name() const noexcept
    {
        return impl_->name();
    }

    std::string_view
    header::name_string() const noexcept
    {
        return impl_->name_string();
    }

    std::string_view
    header::value() const noexcept
    {
        return impl_->value();
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

    headers::headers(headers&& other) noexcept : owned_(std::move(other.owned_)), p_(other.p_)
    {
        other.p_ = nullptr;
    }

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
        get_impl(*this).clear();
    }

    // 两个 operator[] 直接转发给 beast：其 const 重载恰是「find → 无则空 view →
    // 否则第一个匹配值」，与本库语义逐字一致，不必再包一层。
    std::string_view
    headers::operator[](field name) const
    {
        return to_sv(get_impl(*this)[enum_conv::to_field(name)]);
    }

    std::string_view
    headers::operator[](std::string_view name) const
    {
        return to_sv(get_impl(*this)[name]);
    }

    std::string_view
    headers::at(field name) const
    {
        return to_sv(get_impl(*this).at(enum_conv::to_field(name)));
    }

    std::string_view
    headers::at(std::string_view name) const
    {
        return to_sv(get_impl(*this).at(name));
    }

    void
    headers::set(field name, std::string_view value)
    {
        get_impl(*this).set(enum_conv::to_field(name), to_bsv(value));
    }

    void
    headers::set(std::string_view name, std::string_view value)
    {
        get_impl(*this).set(name, to_bsv(value));
    }

    void
    headers::insert(field name, std::string_view value)
    {
        get_impl(*this).insert(enum_conv::to_field(name), to_bsv(value));
    }

    void
    headers::insert(std::string_view name, std::string_view value)
    {
        get_impl(*this).insert(name, to_bsv(value));
    }

    void
    headers::erase(field name)
    {
        get_impl(*this).erase(enum_conv::to_field(name));
    }

    void
    headers::erase(std::string_view name)
    {
        get_impl(*this).erase(name);
    }

    bool
    headers::has(field name) const
    {
        auto const& f = get_impl(*this);
        return f.find(enum_conv::to_field(name)) != f.end();
    }

    bool
    headers::has(std::string_view name) const
    {
        auto const& f = get_impl(*this);
        return f.find(name) != f.end();
    }

    std::size_t
    headers::count(field name) const
    {
        return get_impl(*this).count(enum_conv::to_field(name));
    }

    std::size_t
    headers::count(std::string_view name) const
    {
        return get_impl(*this).count(name);
    }

    std::vector<std::string_view>
    headers::values(field name) const
    {
        return collect(get_impl(*this), enum_conv::to_field(name));
    }

    std::vector<std::string_view>
    headers::values(std::string_view name) const
    {
        return collect(get_impl(*this), name);
    }

    // 定义在 .cpp 的协程：帧在此实例化，公共头里就看不到任何 Boost 类型。帧只存引用
    // 与指针，故借用调用方的字段集合。产出 header const&，那一个 header 沿链复用。
    std::generator<header const&>
    headers::fields() const
    {
        auto const& f = get_impl(*this);

        auto h = header::impl::make();
        for (auto it = f.begin(), last = f.end(); it != last; ++it)
        {
            get_impl(h).rebind(it);
            co_yield h;
        }
    }

    std::size_t
    headers::size() const
    {
        return get_impl(*this).field_count();
    }

    bool
    headers::empty() const
    {
        return size() == 0;
    }

    void
    headers::merge(headers const& other)
    {
        auto& dst = get_impl(*this);
        // 遍历 other 自己的 beast 迭代器。merge 只做 insert（不删节点），故自合并
        // （other == *this）也安全：list 的 end 哨兵节点在插入时不动。
        auto const& src = get_impl(other);
        for (auto it = src.begin(), last = src.end(); it != last; ++it)
        {
            // 必须用 name_string()：非标准头的 name() 是 field::unknown，传给
            // insert(field, ...) 会被 beast 拒绝；标准头的 name_string() 就是该枚举的
            // 规范拼写，故结果与 insert(field, ...) 一致。
            dst.insert(to_sv(it->name_string()), to_sv(it->value()));
        }
    }

} // namespace httplib
