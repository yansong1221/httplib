#pragma once
#include "httplib/config.hpp"
#include <boost/beast/http/fields.hpp>
#include <boost/json/serializer.hpp>
#include <boost/json/stream_parser.hpp>
#include <boost/json/value.hpp>

namespace httplib::body
{
    namespace json = boost::json;

    struct json_body
    {
        using value_type = json::value;

        struct writer
        {
            using const_buffers_type = net::const_buffer;

            explicit writer(http::fields const&, value_type const& body);
            writer(writer&&) noexcept = default;
            writer& operator=(writer&&) noexcept = default;

            void init(boost::system::error_code& ec);

            boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec);

          private:
            json::serializer serializer_;
            char buffer_[32768];
        };

        struct reader
        {
            explicit reader(http::fields const&, value_type& body);
            reader(reader&&) noexcept = default;
            reader& operator=(reader&&) noexcept = default;

            void init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec);

            std::size_t put(net::const_buffer const& buffers, boost::system::error_code& ec);
            void finish(boost::system::error_code& ec);

          private:
            json::stream_parser parser_;
            value_type& body_;
        };
    };
} // namespace httplib::body
