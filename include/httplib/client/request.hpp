#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/config.hpp"
#include <boost/beast/http/fields.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace httplib::client
{

    class HTTPLIB_API request
    {
      public:
        request(http::verb method, std::string_view target, http::fields const& headers = http::fields());
        request(http::verb method,
                std::string_view path,
                html::query_params const& params,
                http::fields const& headers = http::fields());

        request(request&&) noexcept;
        request& operator=(request&&) noexcept;
        ~request();

        http::verb method() const;
        void method(http::verb v);
        std::string_view target() const;
        void target(std::string_view t);

        std::string_view operator[](http::field name) const;
        std::string_view operator[](std::string_view name) const;
        std::string_view at(http::field name) const;
        std::string_view at(std::string_view name) const;

        void set(http::field name, std::string_view value);
        void set(std::string_view name, std::string_view value);
        void erase(http::field name);
        void erase(std::string_view name);
        bool has(http::field name) const;
        bool has(std::string_view name) const;

        http::fields& base();
        http::fields const& base() const;

        body::any_body::value_type& body();
        body::any_body::value_type const& body() const;

        void content_length(std::uint64_t n);
        bool keep_alive() const;
        void keep_alive(bool value);

        // ---- body 设置 ----

        void set_body(std::string_view data);
        void set_body(std::string&& data);
        void set_body(boost::json::value&& data);
        void set_body(html::form_data&& data);
        void set_body(html::query_params&& data);
        void set_file_body(fs::path const& path);

        class impl;

      private:
        request(std::shared_ptr<impl> impl);
        friend impl&
        get_impl(request& self)
        {
            return *self.impl_;
        }
        friend impl const&
        get_impl(request const& self)
        {
            return *self.impl_;
        }
        std::shared_ptr<impl> impl_;
    };

} // namespace httplib::client
