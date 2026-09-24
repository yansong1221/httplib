#pragma once
#include "compress/compressor.hpp"
#include "httplib/config.hpp"
#include <boost/asio/buffer.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/optional.hpp>
#include <cstdint>
#include <utility>

namespace httplib::body
{
    /** Body 包装器：按 Content-Encoding 对 Inner 的 body 做透明解码/编码。

        读取时（reader）把线上压缩字节喂给 compressor，拉出解码结果再转发给
        `Inner::reader`；写入时（writer）反向。`Content-Encoding` 未知或为 identity
        时 compressor 为空，退化为对 `Inner` 的纯转发，因此本包装对任意 Inner 都零成本。

        解压后的大小上限通过 @ref reader::set_limit 注入（0 表示不限），在转发给
        `Inner` 的同时做记账，用于解压炸弹防护。
    */
    template <class Inner>
    struct compressed_body
    {
        using value_type = typename Inner::value_type;
        using const_buffers_type = net::const_buffer;

        class reader
        {
          public:
            reader(http::fields& h, value_type& b) : header_(h), inner_(h, b) {}
            reader(reader&&) noexcept = default;
            reader& operator=(reader&&) noexcept = default;

            /// 解压后允许的字节数上限；0 表示不限。
            void
            set_limit(std::uint64_t limit)
            {
                limit_ = limit;
            }

            void
            init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
            {
                auto encoding = header_[http::field::content_encoding];
                compressor_ = compress::compressor_factory::instance().create(encoding);
                if (compressor_)
                {
                    compressor_->init(compress::compressor::mode::decode, ec);
                    if (ec)
                    {
                        return;
                    }
                    if (limit_ > 0 && content_length.has_value())
                    {
                        auto len = *content_length;
                        if (len >= limit_)
                        {
                            ec = http::error::body_limit;
                            return;
                        }
                        limit_ -= len;
                    }
                }
                inner_.init(content_length, ec);
            }

            std::size_t
            put(const_buffers_type const& buffers, boost::system::error_code& ec)
            {
                if (!compressor_)
                {
                    return inner_.put(buffers, ec);
                }

                compressor_->write(buffers, true, ec);
                if (ec)
                {
                    return buffers.size();
                }

                auto decoded = compressor_->buffer();
                while (decoded.size() != 0 && !ec)
                {
                    auto bytes = inner_.put(decoded, ec);
                    compressor_->consume(bytes);
                    if (ec == http::error::need_more && bytes > 0)
                    {
                        ec = {};
                        decoded = compressor_->buffer();
                        continue;
                    }
                    if (limit_ > 0)
                    {
                        bytes_ += bytes;
                        if (bytes_ > limit_)
                        {
                            ec = http::error::body_limit;
                            return buffers.size();
                        }
                    }
                    decoded = compressor_->buffer();
                }
                return buffers.size();
            }

            void
            finish(boost::system::error_code& ec)
            {
                if (!compressor_)
                {
                    inner_.finish(ec);
                    return;
                }

                compressor_->finish(ec);
                if (ec)
                {
                    return;
                }

                auto decoded = compressor_->buffer();
                while (decoded.size() != 0 && !ec)
                {
                    auto bytes = inner_.put(decoded, ec);
                    if (ec == http::error::need_more)
                    {
                        ec = {};
                        decoded = advance(decoded, bytes);
                        continue;
                    }
                    if (limit_ > 0)
                    {
                        bytes_ += bytes;
                        if (bytes_ > limit_)
                        {
                            ec = http::error::body_limit;
                            return;
                        }
                    }
                    decoded = advance(decoded, bytes);
                }
                if (!ec)
                {
                    inner_.finish(ec);
                }
            }

          private:
            static const_buffers_type
            advance(const_buffers_type const& buffer, std::size_t bytes)
            {
                return const_buffers_type(static_cast<char const*>(buffer.data()) + bytes, buffer.size() - bytes);
            }

            http::fields& header_;
            Inner::reader inner_;
            compress::compressor::ptr compressor_;
            std::uint64_t limit_ = 0;
            std::uint64_t bytes_ = 0;
        };

        class writer
        {
          public:
            using const_buffers_type = net::const_buffer;

            writer(http::fields& h, value_type& b) : header_(h), inner_(h, b) {}

            void
            init(boost::system::error_code& ec)
            {
                auto encoding = header_[http::field::content_encoding];
                compressor_ = compress::compressor_factory::instance().create(encoding);
                if (compressor_)
                {
                    compressor_->init(compress::compressor::mode::encode, ec);
                    if (ec)
                    {
                        return;
                    }
                }
                inner_.init(ec);
            }

            boost::optional<std::pair<const_buffers_type, bool>>
            get(boost::system::error_code& ec)
            {
                if (!compressor_)
                {
                    return inner_.get(ec);
                }

                compressor_->consume_all();
                for (;;)
                {
                    auto result = inner_.get(ec);
                    if (ec)
                    {
                        return boost::none;
                    }
                    if (!result)
                    {
                        compressor_->finish(ec);
                        if (ec)
                        {
                            return boost::none;
                        }
                        auto buffer = compressor_->buffer();
                        if (buffer.size() != 0)
                        {
                            return {
                                { buffer, false }
                            };
                        }
                        return boost::none;
                    }

                    compressor_->write(net::buffer(result->first), result->second, ec);
                    if (ec)
                    {
                        return boost::none;
                    }
                    auto buffer = compressor_->buffer();
                    if (buffer.size() != 0)
                    {
                        return {
                            { buffer, result->second }
                        };
                    }
                }
            }

          private:
            http::fields& header_;
            Inner::writer inner_;
            compress::compressor::ptr compressor_;
        };
    };
} // namespace httplib::body
