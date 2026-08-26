#pragma once
#include "httplib/config.hpp"
#include "httplib/html/form_data.hpp"
#include "httplib/html/query_params.hpp"
#include <boost/beast/http/fields.hpp>
#include <boost/json/value.hpp>
#include <memory>
#include <string>
#include <string_view>

namespace httplib::client
{

    class HTTPLIB_API response
    {
      public:
        response();
        response(response&&) noexcept;
        response& operator=(response&&) noexcept;
        ~response();

        http::status result() const;
        unsigned result_int() const;
        std::string_view operator[](http::field name) const;
        std::string_view operator[](std::string_view name) const;
        http::fields const& headers() const;
        http::fields& headers();
        http::fields const& base() const;
        http::fields& base();

        // body 转换：按内容类型取响应体（引用，不拷贝）；body 非该类型时抛 std::bad_variant_access
        std::string const& as_string() const;
        boost::json::value const& as_json() const;
        html::form_data const& as_form_data() const;
        html::query_params const& as_query_params() const;

        class impl;

      private:
        response(std::shared_ptr<impl> impl);
        std::shared_ptr<impl> impl_;

        friend impl&
        get_impl(response& self)
        {
            return *self.impl_;
        }
        friend impl const&
        get_impl(response const& self)
        {
            return *self.impl_;
        }
    };

} // namespace httplib::client
