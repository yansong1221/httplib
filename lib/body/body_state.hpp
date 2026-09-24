#pragma once
#include "httplib/config.hpp"
#include "httplib/html/form_data.hpp"
#include "httplib/html/query_params.hpp"
#include <boost/json/value.hpp>
#include <string>
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
        none_tag, // 0: 尚未读取
        empty_tag, // 1: 显式空
        std::string, // 2: string
        boost::json::value, // 3: json
        html::query_params, // 4: query_params
        html::form_data, // 5: form_data
        file_tag>; // 6: file 标记

    /** 方向无关的读取结果（server request / client request / client response 共用）。
    */
    class body_state
    {
      public:
        /// 与 @ref body_variant 分支下标一一对应。
        enum class kind
        {
            none,
            empty,
            string,
            json,
            query_params,
            form_data,
            file,
        };

        bool
        has() const
        {
            return state_.index() != static_cast<std::size_t>(kind::none);
        }

        kind
        type() const
        {
            return static_cast<kind>(state_.index());
        }

        bool
        is_empty() const
        {
            return state_.index() == static_cast<std::size_t>(kind::empty);
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

    static_assert(static_cast<std::size_t>(body_state::kind::none) == 0);
    static_assert(static_cast<std::size_t>(body_state::kind::file) == 6);
} // namespace httplib::body