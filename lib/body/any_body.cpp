#include "body/any_body.hpp"

using namespace httplib::compress;

namespace httplib::body
{
    any_body::writer::writer(http::fields& h, value_type& b) : header_(h), body_(b) {}

    void
    any_body::writer::init(boost::system::error_code& ec)
    {
        auto content_encoding = header_[http::field::content_encoding];

        create_writer(header_, body_);
        compressor_ = compressor_factory::instance().create(content_encoding);

        if (compressor_)
        {
            compressor_->init(compressor::mode::encode);
        }
        proxy_.init(ec);
    }

    boost::optional<std::pair<any_body::writer::const_buffers_type, bool>>
    any_body::writer::get(boost::system::error_code& ec)
    {
        if (!compressor_)
        {
            return proxy_.get(ec);
        }

        compressor_->consume_all();
        for (;;)
        {
            auto result = proxy_.get(ec);
            if (ec)
            {
                return boost::none;
            }
            if (!result)
            {
                compressor_->finish();
                auto buffer = compressor_->buffer();
                if (buffer.size() != 0)
                {
                    return {
                        { buffer, false }
                    };
                }
                return boost::none;
            }

            compressor_->write(net::buffer(result->first), result->second);
            auto buffer = compressor_->buffer();
            if (buffer.size() != 0)
            {
                return {
                    { buffer, result->second }
                };
            }
        }
    }

    any_body::reader::reader(http::fields& h, value_type& b) : header_(h), body_(b) {}

    void
    any_body::reader::init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
    {
        auto content_type = header_[http::field::content_type];
        auto content_encoding = header_[http::field::content_encoding];

        if (!std::holds_alternative<body::empty_body::value_type>(body_))
        {
            create_reader(body_);
        }
        else if (content_type.starts_with("multipart/form-data"))
        {
            create_reader<form_data_body>();
        }
        else if (content_type.starts_with("application/json"))
        {
            create_reader<json_body>();
        }
        else if (content_type.starts_with("application/x-www-form-urlencoded"))
        {
            create_reader<query_params_body>();
        }
        else
        {
            create_reader<string_body>();
        }
        compressor_ = compressor_factory::instance().create(content_encoding);
        if (compressor_)
        {
            compressor_->init(compressor::mode::decode);
        }
        proxy_.init(content_length, ec);
    }

    std::size_t
    any_body::reader::put(const_buffers_type const& buffers, boost::system::error_code& ec)
    {
        if (!compressor_)
        {
            return proxy_.put(buffers, ec);
        }

        compressor_->write(buffers);

        auto decoded_buffer = compressor_->buffer();
        while (decoded_buffer.size() != 0 && !ec)
        {
            auto bytes = proxy_.put(decoded_buffer, ec);
            compressor_->consume(bytes);
            if (ec == http::error::need_more && bytes > 0)
            {
                ec = {};
                decoded_buffer = compressor_->buffer();
                continue;
            }
            decoded_buffer = compressor_->buffer();
        }
        return buffers.size();
    }

    void
    any_body::reader::finish(boost::system::error_code& ec)
    {
        if (!compressor_)
        {
            return proxy_.finish(ec);
        }

        compressor_->finish();
        auto decoded_buffer = compressor_->buffer();
        while (decoded_buffer.size() != 0 && !ec)
        {
            auto bytes = proxy_.put(decoded_buffer, ec);
            if (ec == http::error::need_more)
            {
                ec = {};
                decoded_buffer = net::const_buffer(static_cast<char const*>(decoded_buffer.data()) + bytes,
                                                   decoded_buffer.size() - bytes);
                continue;
            }
            decoded_buffer = net::const_buffer(static_cast<char const*>(decoded_buffer.data()) + bytes,
                                               decoded_buffer.size() - bytes);
        }
        if (!ec)
        {
            proxy_.finish(ec);
        }
    }

} // namespace httplib::body
