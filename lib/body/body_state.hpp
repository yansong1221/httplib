#pragma once
#include "httplib/body_type.hpp"
#include "httplib/config.hpp"
#include "httplib/html/form_data.hpp"
#include "httplib/html/query_params.hpp"
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
        公共 API 以 `as_*` / `take_*` 取回。
    */
    using body_variant = std::variant<
        none_tag, // 尚未读取
        empty_tag, // 显式空
        std::string, // string
        boost::json::value, // json
        html::query_params, // query_params
        html::form_data, // form_data
        file_tag>; // file 标记

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
                    else if constexpr (std::is_same_v<T, html::query_params>)
                    {
                        return kind::query_params;
                    }
                    else if constexpr (std::is_same_v<T, html::form_data>)
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

        std::string const&
        as_string() const
        {
            return std::get<std::string>(state_);
        }

        boost::json::value const&
        as_json() const
        {
            return std::get<boost::json::value>(state_);
        }

        html::query_params const&
        as_query_params() const
        {
            return std::get<html::query_params>(state_);
        }

        html::form_data const&
        as_form_data() const
        {
            return std::get<html::form_data>(state_);
        }

        void
        set_empty()
        {
            state_ = empty_tag {};
        }

        void
        set_file()
        {
            state_ = file_tag {};
        }

        void
        set_string(std::string v)
        {
            state_ = std::move(v);
        }

        void
        set_json(boost::json::value v)
        {
            state_ = std::move(v);
        }

        void
        set_query_params(html::query_params v)
        {
            state_ = std::move(v);
        }

        void
        set_form_data(html::form_data v)
        {
            state_ = std::move(v);
        }

        void
        reset()
        {
            state_ = none_tag {};
        }

        std::string
        take_string()
        {
            return std::get<std::string>(std::move(state_));
        }

        boost::json::value
        take_json()
        {
            return std::get<boost::json::value>(std::move(state_));
        }

        html::query_params
        take_query_params()
        {
            return std::get<html::query_params>(std::move(state_));
        }

        html::form_data
        take_form_data()
        {
            return std::get<html::form_data>(std::move(state_));
        }

      private:
        body_variant state_;
    };
} // namespace httplib::body