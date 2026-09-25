#pragma once
#include "httplib/body_type.hpp"
#include "httplib/config.hpp"
#include "httplib/form_data.hpp"
#include "httplib/query_params.hpp"
#include <boost/beast/http/fields.hpp>
#include <boost/json/value.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace httplib::client
{

    class HTTPLIB_API request
    {
      public:
        request(http::verb method, std::string_view target, http::fields const& headers = http::fields());
        request(http::verb method,
                std::string_view path,
                httplib::query_params const& params,
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

        void insert(http::field name, std::string_view value);
        void insert(std::string_view name, std::string_view value);

        void erase(http::field name);
        void erase(std::string_view name);
        bool has(http::field name) const;
        bool has(std::string_view name) const;

        http::fields& base();
        http::fields const& base() const;

        std::string const& as_string() const;
        boost::json::value const& as_json() const;
        httplib::form_data const& as_form_data() const;
        httplib::query_params const& as_query_params() const;

        body_type type() const;

        void content_length(std::uint64_t n);
        bool keep_alive() const;
        void keep_alive(bool value);

        // ---- body 设置 ----

        void set_body(std::string_view data, std::string_view content_type);
        void set_body(std::string&& data, std::string_view content_type);
        void set_body(boost::json::value&& data);
        void set_body(httplib::form_data&& data);
        void set_body(httplib::query_params&& data);
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
