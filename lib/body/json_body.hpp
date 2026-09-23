#pragma once
#include "httplib/config.hpp"
#include <boost/beast/http/fields.hpp>
#include <boost/json/error.hpp>
#include <boost/json/monotonic_resource.hpp>
#include <boost/json/serializer.hpp>
#include <boost/json/stream_parser.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <cstdint>

namespace httplib::body
{
    namespace json = boost::json;

    struct json_body
    {
        using value_type = json::value;

        struct writer
        {
            using const_buffers_type = net::const_buffer;

            explicit writer(http::fields const&, value_type const& body) { serializer_.reset(&body); }
            writer(writer&&) noexcept = default;
            writer& operator=(writer&&) noexcept = default;

            void
            init(boost::system::error_code& ec)
            {
                ec = {};
            }

            boost::optional<std::pair<const_buffers_type, bool>>
            get(boost::system::error_code& ec)
            {
                ec = {};
                auto const len = serializer_.read(buffer_, sizeof(buffer_));
                return std::make_pair(net::const_buffer(len.data(), len.size()), !serializer_.done());
            }

          private:
            json::serializer serializer_;
            char buffer_[32768];
        };

        struct reader
        {
            explicit reader(http::fields const&, value_type& body) : body_(body) {}
            reader(reader&&) noexcept = default;
            reader& operator=(reader&&) noexcept = default;

            void
            init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
            {
                if (content_length)
                {
                    static constexpr std::uint64_t max_json_size = 10 * 1024 * 1024;
                    auto alloc_sz = std::min(*content_length, max_json_size);
                    parser_.reset(json::make_shared_resource<json::monotonic_resource>(alloc_sz));
                }
                ec = {};
            }

            std::size_t
            put(net::const_buffer const& buffers, boost::system::error_code& ec)
            {
                ec = {};
                return parser_.write_some(static_cast<char const*>(buffers.data()), buffers.size(), ec);
            }

            void
            finish(boost::system::error_code& ec)
            {
                ec = {};
                if (parser_.done())
                {
                    body_ = parser_.release();
                }
                else
                {
                    ec = boost::json::error::incomplete;
                }
            }

          private:
            json::stream_parser parser_;
            value_type& body_;
        };
    };
} // namespace httplib::body
