#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/config.hpp"
#include <memory>
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
        body::any_body::value_type& body();
        body::any_body::value_type const& body() const;
        unsigned version() const;
        bool keep_alive() const;
        std::string_view reason() const;

        class impl;

      private:
        response(std::shared_ptr<impl> impl);
        std::shared_ptr<impl> impl_;
    };

} // namespace httplib::client
