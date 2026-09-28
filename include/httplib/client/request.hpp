#pragma once
#include "httplib/config.hpp"
#include "httplib/form_data.hpp"
#include "httplib/headers.hpp"
#include "httplib/query_params.hpp"
#include <boost/json/value.hpp>
#include <cstddef>
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
        request(httplib::method method, std::string_view target, httplib::headers const& headers = {});
        request(httplib::method method,
                std::string_view path,
                httplib::query_params const& params,
                httplib::headers const& headers = {});

        request(request&&) noexcept;
        request& operator=(request&&) noexcept;
        ~request();

        httplib::method method() const;
        void method(httplib::method v);
        std::string_view target() const;
        void target(std::string_view t);

        std::string_view operator[](httplib::field name) const;
        std::string_view operator[](std::string_view name) const;
        std::string_view at(httplib::field name) const;
        std::string_view at(std::string_view name) const;

        void set(httplib::field name, std::string_view value);
        void set(std::string_view name, std::string_view value);

        void insert(httplib::field name, std::string_view value);
        void insert(std::string_view name, std::string_view value);

        void erase(httplib::field name);
        void erase(std::string_view name);
        bool has(httplib::field name) const;
        bool has(std::string_view name) const;
        /// 该请求头出现的次数（1 = 无重复头）。
        std::size_t count(httplib::field name) const;
        std::size_t count(std::string_view name) const;

        void replace(httplib::headers const& fields);

        /// 全部请求头。返回的是「借用」视图：写入直接落到本请求上。
        httplib::headers base();
        httplib::headers base() const;

        void content_length(std::uint64_t n);
        bool keep_alive() const;
        void keep_alive(bool value);

        // ---- body 设置 ----

        void set_body(std::string_view data, std::string_view content_type);
        void set_body(std::string&& data, std::string_view content_type);
        void set_body(boost::json::value&& data);
        void set_body(httplib::form_data&& data);
        void set_body(httplib::query_params&& data);
        void set_file_body(fs::path const& path, boost::system::error_code& ec);

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
