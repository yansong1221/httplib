#pragma once
#include "httplib/config.hpp"
#include "httplib/enums.hpp"
#include <cstddef>
#include <iterator>
#include <memory>
#include <string_view>
#include <vector>

namespace httplib
{

    namespace detail
    {
        /// 库内部访问 headers 私有存储的桥接结构体，定义在 lib/headers_impl.hpp。
        class headers_access;
    } // namespace detail

    /// 单个头字段的只读快照。
    struct HTTPLIB_API header
    {
        header(field name, std::string_view name_string, std::string_view value) noexcept
            : name_(name)
            , name_string_(name_string)
            , value_(value)
        {
        }

        /// 标准头的字段名；非标准头为 field::unknown，此时以 name_string() 为准。
        field
        name() const noexcept
        {
            return name_;
        }

        /// 字段名原文（线上写法），对非标准头同样有效。
        std::string_view
        name_string() const noexcept
        {
            return name_string_;
        }

        /// 该字段的值。重复头（Set-Cookie 等）会出现多条 header，各持一个值。
        std::string_view
        value() const noexcept
        {
            return value_;
        }

      private:
        field name_ = field::unknown;
        std::string_view name_string_;
        std::string_view value_;
    };

    /// header_iterator::operator-> 的中转。*it 返回按值快照，没有可指向的存储，
    /// 故让 `it->value()` 走一个持有快照的对象。只读，仅在完整表达式内有效。
    class HTTPLIB_API header_accessor
    {
      public:
        explicit header_accessor(header h) noexcept : h_(h) {}

        header const*
        operator->() const noexcept
        {
            return &h_;
        }

      private:
        header h_;
    };

    /// HTTP 头字段集合。
    ///
    /// 语义对齐 std::string_view 的借用/自有两态：
    ///  - 自有：默认构造的 headers 持有自己的字段集合，可自由增删。
    ///  - 借用：库内部把消息（request/response）自己的字段集合以视图暴露出来
    ///    （见 request::base() / response::base() / client::response::headers()）。
    ///    写入借用集合会直接落到消息上；拷贝一个借用 headers 得到的仍是
    ///    指向同一集合的视图，而非快照。
    class HTTPLIB_API headers
    {
      private:
        /// 内部实现类型，在 lib/headers_impl.hpp 补全。
        class impl;

      public:
        /// 全部请求头/响应头的只读遍历器。按线序，重复头展开成多条。
        ///
        /// *it 返回 header **按值**（快照），不是引用；里面的 string_view 指向底层
        /// 存储，所以底层集合被销毁或改写后即失效。迭代期间增删字段会让迭代器失效。
        /// 底层字段是一条链，故 ++ 是 O(字段数)。
        class const_iterator
        {
          public:
            using iterator_category = std::forward_iterator_tag;
            using value_type = header;
            using difference_type = std::ptrdiff_t;
            using pointer = void;
            /// operator* 返回按值的快照，故 reference 就是 header 本身。
            using reference = header;

            /// 默认构造出的是「空值」迭代器（singular），只和自己相等，不能解引用。
            const_iterator() noexcept = default;
            const_iterator(const_iterator const&) noexcept = default;
            const_iterator& operator=(const_iterator const&) noexcept = default;
            ~const_iterator() = default;

            header operator*() const;
            /// 返回持有当前快照的中转，使 `it->name()` / `it->value()` 可用；只读。
            header_accessor operator->() const;
            const_iterator& operator++();
            const_iterator operator++(int);

            /// 同时比集合和序号：这样两个空值迭代器相等，而空值迭代器和任何
            /// 真实 end() 都不相等（后者带着非空的 fields_）。
            friend bool
            operator==(const_iterator const& a, const_iterator const& b) noexcept
            {
                return a.fields_ == b.fields_ && a.index_ == b.index_;
            }

            friend bool
            operator!=(const_iterator const& a, const_iterator const& b) noexcept
            {
                return !(a == b);
            }

          private:
            friend class headers;

            const_iterator(impl const* fields, std::size_t index) noexcept : fields_(fields), index_(index) {}

            impl const* fields_ = nullptr;
            std::size_t index_ = 0;
        };

        using iterator = const_iterator;

        headers();
        headers(headers const& other);
        headers(headers&& other) noexcept;
        headers& operator=(headers const& other);
        headers& operator=(headers&& other) noexcept;
        ~headers();

        void clear();

        /// 取第一个值；字段不存在返回空 string_view。
        std::string_view operator[](field name) const;
        std::string_view operator[](std::string_view name) const;
        /// 取第一个值；字段不存在抛 std::out_of_range。
        std::string_view at(field name) const;
        std::string_view at(std::string_view name) const;

        /// 覆盖写入（同名字段原有值全部丢弃）。
        void set(field name, std::string_view value);
        void set(std::string_view name, std::string_view value);
        /// 追加一个同名字段，不影响已有值。
        void insert(field name, std::string_view value);
        void insert(std::string_view name, std::string_view value);

        /// 清除该字段名的全部值。
        void erase(field name);
        void erase(std::string_view name);

        bool has(field name) const;
        bool has(std::string_view name) const;
        /// 该字段名出现的次数（1 = 无重复头）。
        std::size_t count(field name) const;
        std::size_t count(std::string_view name) const;
        /// 该字段名的全部值，按出现顺序。
        std::vector<std::string_view> values(field name) const;
        std::vector<std::string_view> values(std::string_view name) const;
        /// 全量快照，按线序；重复头展开成多条。
        std::vector<header> all() const;

        /// 只读遍历。按线序，重复头展开成多条（Set-Cookie 等会出现多次）。
        const_iterator begin() const noexcept;
        const_iterator end() const noexcept;
        const_iterator cbegin() const noexcept;
        const_iterator cend() const noexcept;

        std::size_t size() const;
        bool empty() const;

        /// 把 other 的字段全部追加进来（同名共存）。
        void merge(headers const& other);

      private:
        struct borrowed_tag
        {
        };

        /// 借用模式构造：只记指针，不分配自有存储。
        ///
        /// 必须定义在 lib/ 里而不能内联：只要在类内定义任何构造函数，MSVC 就会在
        /// 本头实例化 owned_ 的析构（EH 清理路径），而 impl 在此处是不完整类型。
        /// 所以这里只留声明。
        explicit headers(borrowed_tag, impl& borrowed) noexcept;

        /// 库内部用：以借用模式包装一个既有的字段集合。参数是不完整的 impl，
        /// 调用方拿不到它，因此只有库内能调用。
        static headers borrow(impl& borrowed) noexcept;

        friend class detail::headers_access;

        // 不变式（二者必居其一，移动后源两者皆空）：
        //   自有：owned_ != nullptr 且 p_ == owned_.get()
        //   借用：owned_ == nullptr 且 p_ != nullptr
        impl* p_ = nullptr;           ///< 借用模式指向外部集合，自有模式等于 owned_.get()
        std::unique_ptr<impl> owned_; ///< 仅自有模式非空
    };

    /// 等价于 headers::const_iterator，便于在 headers 之外提到。
    using header_iterator = headers::const_iterator;

} // namespace httplib
