#pragma once
#include "httplib/body_type.hpp"
#include "httplib/config.hpp"
#include "httplib/form_data.hpp"
#include "httplib/query_params.hpp"
#include <boost/json/value.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace httplib::body
{
    /// 尚未读取（默认态）。
    struct none_tag
    {
    };

    /// 显式空 body（无 body，is_empty() == true）。
    struct empty_tag
    {
    };

    /// 文件型 body 的标记（set_file_body：只标记，不存路径）。
    struct file_tag
    {
    };

    /** 物化后的业务 body 结果容器。

        与传输层（`http::buffer_body`）解耦：同一时刻只承载一种已解析的业务类型，
        用共用体 `std::variant` 存储（取代旧 `any_body::value_type`），由 sink 写入，
        内部以模板存取：`as<T>()` / `take<T>()` 取回、`set<T>()` 写入。
    */
    using body_variant = std::variant<none_tag,              // 尚未读取
                                      empty_tag,             // 显式空
                                      std::string,           // string
                                      boost::json::value,    // json
                                      httplib::query_params, // query_params
                                      httplib::form_data,    // form_data
                                      file_tag>;             // file 标记

    /** 方向无关的读取结果（server request / client request / client response 共用）。
     */
    class body_state
    {
      public:
        using kind = httplib::body_type;

        bool
        has() const
        {
            return type() != kind::none;
        }

        /// 由 variant 当前分支映射得到，不依赖分支声明顺序。
        kind
        type() const
        {
            return std::visit(
                [](auto const& value) -> kind
                {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, none_tag>)
                    {
                        return kind::none;
                    }
                    else if constexpr (std::is_same_v<T, empty_tag>)
                    {
                        return kind::empty;
                    }
                    else if constexpr (std::is_same_v<T, std::string>)
                    {
                        return kind::string;
                    }
                    else if constexpr (std::is_same_v<T, boost::json::value>)
                    {
                        return kind::json;
                    }
                    else if constexpr (std::is_same_v<T, httplib::query_params>)
                    {
                        return kind::query_params;
                    }
                    else if constexpr (std::is_same_v<T, httplib::form_data>)
                    {
                        return kind::form_data;
                    }
                    else
                    {
                        return kind::file;
                    }
                },
                state_);
        }

        bool
        is_empty() const
        {
            return type() == kind::empty;
        }

        /// 按类型取回当前分支的 const 引用（类型需为 variant 的某个存储类型）。
        template <class T>
        T const&
        as() const
        {
            return std::get<T>(state_);
        }

        /// 按类型写入并覆盖当前分支；tag 类型不带参数（如 `set<empty_tag>()`）。
        template <class T>
        void
        set(T value = {})
        {
            state_ = std::move(value);
        }

        void
        reset()
        {
            state_ = none_tag {};
        }

        /// 按类型取走当前分支并移交所有权（variant 仍保留该分支，值为 moved-from）。
        template <class T>
        T
        take()
        {
            return std::get<T>(std::move(state_));
        }

      private:
        body_variant state_;
    };
} // namespace httplib::body