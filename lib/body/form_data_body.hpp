#pragma once
#include "httplib/config.hpp"
#include "httplib/html/form_data.hpp"
#include <array>
#include <boost/algorithm/string/trim.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/fields.hpp>
#include <fstream>

namespace httplib::body
{

    class form_data_body
    {
      public:
        using value_type = html::form_data;

        class writer
        {
          public:
            using const_buffers_type = net::const_buffer;

            writer(http::fields const& h, value_type& b);

            void init(boost::system::error_code& ec);
            boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec);

          private:
            value_type& body_;
            int field_data_index_ = 0;
            beast::flat_buffer buffer_;

            std::ifstream file_stream_;
            std::uintmax_t file_remaining_ = 0;
            static constexpr std::size_t file_buf_size_ = 8192;
            std::array<char, file_buf_size_> file_buf_;

            enum class step
            {
                header,
                content,
                content_end,
                eof
            };
            step step_ = step::header;
        };

        //--------------------------------------------------------------------------

        class reader
        {
          public:
            using const_buffers_type = net::const_buffer;

            reader(http::fields const& h, value_type& b);

            void init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec);

            std::size_t put(const_buffers_type const& buffers, boost::system::error_code& ec);

            void finish(boost::system::error_code& ec);

          private:
            value_type& body_;
            std::string content_type_;
            std::string boundary_;
            enum class step
            {
                boundary_line,
                boundary_header,
                boundary_content,
                eof
            };
            step step_ = step::boundary_line;
            html::form_data::field field_data_;

            // Bytes received but not yet committed: they may be the split
            // half of a boundary delimiter ("\r\n--boundary..."), so they are
            // held back until the next put() can disambiguate them.
            std::string pending_;

            std::ofstream file_stream_;
            fs::path current_file_path_;
            std::uint64_t file_bytes_written_ = 0;
            void write_content(std::string_view data, boost::system::error_code& ec);
        };
    };
} // namespace httplib::body
