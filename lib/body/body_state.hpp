#pragma once
#include "body/any_body.hpp"
#include <boost/json/value.hpp>
#include <optional>
#include <string>

namespace httplib::body
{
    /// 物化后的 body 值类型（`any_body` 的 variant 实例）。
    using body_value = any_body::value_type;

    /// 方向无关的 body 访问原语：直接作用于 `any_body::value_type`。
    /// 任意持有 body 的容器（server::request / client::response / client::request）
    /// 都复用这一份语义，避免各处重复 std::get / is_body_type。
    namespace access
    {
        template <typename Body>
        bool
        is(body_value const& v)
        {
            return v.template is_body_type<Body>();
        }

        inline std::string const&
        as_string(body_value const& v)
        {
            return std::get<std::string>(v);
        }

        inline boost::json::value const&
        as_json(body_value const& v)
        {
            return std::get<boost::json::value>(v);
        }

        inline html::form_data const&
        as_form_data(body_value const& v)
        {
            return std::get<html::form_data>(v);
        }

        inline html::query_params const&
        as_query_params(body_value const& v)
        {
            return std::get<html::query_params>(v);
        }

        template <typename T>
        T
        take(body_value& v)
        {
            return std::move(std::get<T>(v));
        }
    } // namespace access

    /// 持有物化后 body 的状态容器。
    /// server::request 与 client::response 共用，替代各自私有的 optional<message>。
    class body_state
    {
      public:
        bool
        ready() const
        {
            return body_.has_value();
        }

        template <typename Body>
        bool
        is() const
        {
            return body_ && access::is<Body>(*body_);
        }

        template <typename T>
        T
        take()
        {
            auto value = access::take<T>(*body_);
            body_.reset();
            return value;
        }

        std::string const&
        as_string() const
        {
            if (!body_)
            {
                throw std::bad_variant_access {};
            }
            return access::as_string(*body_);
        }

        boost::json::value const&
        as_json() const
        {
            if (!body_)
            {
                throw std::bad_variant_access {};
            }
            return access::as_json(*body_);
        }

        html::form_data const&
        as_form_data() const
        {
            if (!body_)
            {
                throw std::bad_variant_access {};
            }
            return access::as_form_data(*body_);
        }

        html::query_params const&
        as_query_params() const
        {
            if (!body_)
            {
                throw std::bad_variant_access {};
            }
            return access::as_query_params(*body_);
        }

        void
        assign(body_value v)
        {
            body_ = std::move(v);
        }

      private:
        std::optional<body_value> body_;
    };
} // namespace httplib::body
