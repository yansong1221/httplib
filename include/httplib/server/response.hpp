#pragma once
#include "httplib/body/any_body.hpp"
#include "httplib/html/form_data.hpp"
#include "httplib/server/server_fwd.hpp"
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/json/value.hpp>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace httplib::server
{

    class HTTPLIB_API response
    {
      public:
        ~response();

        http::fields& base();
        http::fields const& base() const;

        std::string_view operator[](http::field name) const;
        std::string_view operator[](std::string_view name) const;
        std::string_view at(http::field name) const;
        std::string_view at(std::string_view name) const;

        void set(http::field name, std::string_view value);
        void set(std::string_view name, std::string_view value);

        bool has(http::field name) const;
        bool has(std::string_view name) const;
        void erase(http::field name);
        void erase(std::string_view name);

        http::status result() const;
        unsigned result_int() const;

        void set_empty_content(http::status status);
        void set_error_content(http::status status);

        void
        set_string_content(std::string_view data, std::string_view content_type, http::status status = http::status::ok)
        {
            set_string_content(std::string(data), content_type, status);
        }
        void set_string_content(std::string&& data,
                                std::string_view content_type,
                                http::status status = http::status::ok);

        void
        set_json_content(boost::json::value const& data, http::status status = http::status::ok)
        {
            set_json_content(boost::json::value(data), status);
        }
        void set_json_content(boost::json::value&& data, http::status status = http::status::ok);
        void set_file_content(fs::path const& path, http::fields const& req_header = {});
        void set_form_data_content(std::vector<html::form_data::field>&& data);

        void set_redirect(std::string_view url, http::status status = http::status::moved_permanently);

        class impl;

        std::unique_ptr<server::sse_writer> create_sse_writer();
        std::unique_ptr<server::ndjson_writer> create_ndjson_writer();

        chunk_writer* get_chunk_writer();

        impl* get_impl();
        impl const* get_impl() const;

      protected:
        response(std::unique_ptr<impl>&& _impl);

      private:
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server