#pragma once
#include "httplib/config.hpp"
#include "httplib/html/query_params.hpp"
#include <boost/asio/buffer.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/optional.hpp>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace httplib::body
{
    struct query_params_body
    {
        using value_type = html::query_params;

        struct writer
        {
            using const_buffers_type = net::const_buffer;

            writer(http::fields const&, value_type const& body) : body_(body) {}

            void
            init(boost::system::error_code& ec)
            {
                ec = {};
                buffer_ = body_.encoded();
            }

            boost::optional<std::pair<const_buffers_type, bool>>
            get(boost::system::error_code& ec)
            {
                ec = {};
                return {
                    { net::buffer(buffer_), false }
                };
            }

          private:
            value_type const& body_;
            std::string buffer_;
        };

        struct reader
        {
            reader(http::fields const&, value_type& body) : body_(body) {}

            void
            init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
            {
                if (content_length)
                {
                    buffer_.reserve(*content_length);
                }
                ec = {};
            }

            std::size_t
            put(net::const_buffer const& buffers, boost::system::error_code& ec)
            {
                ec = {};
                buffer_.append((char const*)buffers.data(), buffers.size());
                return buffers.size();
            }

            void
            finish(boost::system::error_code& ec)
            {
                ec = {};
                if (!body_.decode(buffer_))
                {
                    ec = http::error::unexpected_body;
                }
            }

          private:
            value_type& body_;
            std::string buffer_;
        };
    };
} // namespace httplib::body
