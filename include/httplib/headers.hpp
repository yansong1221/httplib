#pragma once
#include "httplib/config.hpp"
#include "httplib/enums.hpp"
#include <cstddef>
#include <generator>
#include <memory>
#include <string_view>
#include <vector>

namespace httplib
{

    /**
     * \brief 单个头字段的只读借用视图。
     * \details
     * 只能由 \ref headers::fields 产出，内部持有底层集合的一个
     * \c http::fields::const_iterator，名字与值按需转发给 beast，不切段拷贝字符串。
     *
     * \par 生命周期
     * 视图借用底层集合：集合被 \ref headers::erase 或销毁后，本对象即失效。拷贝一次即
     * 一次堆分配，故遍历时写 \c auto const& f，让 \ref headers::fields 协程帧里那一个
     * 复用。
     */
    class HTTPLIB_API header
    {
      public:
        class impl;

        header(header const& other);
        header(header&& other) noexcept;
        header& operator=(header const& other);
        header& operator=(header&& other) noexcept;
        ~header();

        /// \brief 标准头的字段名。
        /// \return 非标准头为 \c field::unknown ，此时以 \ref name_string 为准。
        field name() const noexcept;

        /// \brief 字段名原文，即线上写法；对非标准头同样有效。
        std::string_view name_string() const noexcept;

        /// \brief 该字段的值。重复头（\c Set-Cookie 等）会出现多条 \ref header ，
        /// 各持一个值。
        std::string_view value() const noexcept;

      private:
        header() noexcept;
        std::unique_ptr<impl> impl_;

        friend impl&
        get_impl(header& h) noexcept
        {
            return *h.impl_;
        }

        friend impl const&
        get_impl(header const& h) noexcept
        {
            return *h.impl_;
        }
    };

    /**
     * \brief HTTP 头字段集合。
     * \details
     * 语义对齐 \c std::string_view 的借用 / 自有两态：
     *  - 自有：默认构造的对象持有自己的字段集合，可自由增删。
     *  - 借用：库内部把消息（\c request / \c response）自己的字段集合以视图暴露出来
     *    （见 \c request::base() 、 \c response::base() ）。写入借用集合直接落到消息
     *    上；拷贝借用集合得到的仍是同一集合的视图，而非快照。
     */
    class HTTPLIB_API headers
    {
      public:
        class impl;

        headers();
        headers(headers const& other);
        headers(headers&& other) noexcept;
        headers& operator=(headers const& other);
        headers& operator=(headers&& other) noexcept;
        ~headers();

        void clear();

        /// \brief 取第一个值。
        /// \return 字段不存在时为空 \c string_view 。
        std::string_view operator[](field name) const;
        std::string_view operator[](std::string_view name) const;
        /// \brief 取第一个值。
        /// \return 字段不存在时抛 \c std::out_of_range 。
        std::string_view at(field name) const;
        std::string_view at(std::string_view name) const;

        /// \brief 覆盖写入，同名字段原有值全部丢弃。
        void set(field name, std::string_view value);
        void set(std::string_view name, std::string_view value);
        /// \brief 追加一个同名字段，不影响已有值。
        void insert(field name, std::string_view value);
        void insert(std::string_view name, std::string_view value);

        /// \brief 清除该字段名的全部值。
        void erase(field name);
        void erase(std::string_view name);

        bool has(field name) const;
        bool has(std::string_view name) const;
        /// \brief 该字段名出现的次数，1 表示无重复头。
        std::size_t count(field name) const;
        std::size_t count(std::string_view name) const;
        /// \brief 该字段名的全部值，顺序即线上顺序。
        std::vector<std::string_view> values(field name) const;
        std::vector<std::string_view> values(std::string_view name) const;

        /**
         * \brief 全部字段的生成器，按线序，重复头展开成多条。
         * \details
         * 返回生成器而非迭代器：beast 的字段集合是侵入式链表，其 \c const_iterator
         * 虽是 \c basic_fields 的 public typedef，但指向的元素类型是私有 typedef，
         * 无法在本头里命名出完整的字段视图类型；改用协程后遍历主体落在 \c lib/ 的
         * 协程帧里，那里可以直接用 beast 迭代器（\c ++ 为 O(1)），而公共头不出现任何
         * Boost 类型。
         *
         * 产出 \c header const& ：被观察的是协程帧内那一个 \ref header ，沿链复用，
         * 不逐字段分配。
         *
         * \par 借用约束
         * 生成器借用 \c *this ，\c *this 先死则生成器悬垂；协程帧归生成器所有而非归
         * 其迭代器，故 \c h.fields().begin() 里那个临时生成器一析构，迭代器即悬垂。
         * range-for 无妨（会延长临时量寿命），但 \c begin() 与 \c end() 分开写必须先
         * \c auto g = h.fields(); 。
         *
         * \note 迭代器只有 \c input 语义（由生成器定义使然），多趟遍历及
         * \c std::distance 、 \c std::find_if 一类算法不可用。
         *
         * \par 用法
         * \code
         * for (auto const& f : req.base().fields()) { ... }
         * \endcode
         */
        std::generator<header const&> fields() const;

        std::size_t size() const;
        bool empty() const;

        /// \brief 把 \p other 的字段全部追加进来，同名字段共存。
        void merge(headers const& other);

      private:
        struct borrowed_tag
        {
        };

        /// 必须定义在 lib/ 里：类内只要定义任何构造函数，MSVC 就会在本头实例化
        /// owned_ 的析构（EH 清理路径），而 impl 在此处不完整。
        explicit headers(borrowed_tag, impl& borrowed) noexcept;

        /// 库内部用：参数是不完整的 impl，调用方拿不到它，故只有库内能调用。
        static headers borrow(impl& borrowed) noexcept;

        friend impl&
        get_impl(headers& self) noexcept
        {
            return *self.p_;
        }
        friend impl const&
        get_impl(headers const& self) noexcept
        {
            return *self.p_;
        }

        // 不变式（二者必居其一，移动后源两者皆空）：
        //   自有：owned_ != nullptr 且 p_ == owned_.get()
        //   借用：owned_ == nullptr 且 p_ != nullptr
        impl* p_ = nullptr;
        std::unique_ptr<impl> owned_;
    };

} // namespace httplib
