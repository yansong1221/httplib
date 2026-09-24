#include "body/codec.hpp"
#include <boost/beast/http/error.hpp>
#include <cstring>
#include <algorithm>

namespace httplib::body
{
    namespace
    {
        compress::compressor::ptr
        make_compressor(std::string_view encoding, compress::compressor::mode mode, boost::system::error_code& ec)
        {
            ec = {};
            auto compressor = compress::compressor_factory::instance().create(std::string(encoding));
            if (!compressor)
            {
                // identity / 未知编码：透传。
                return nullptr;
            }
            compressor->init(mode, ec);
            if (ec)
            {
                return nullptr;
            }
            return compressor;
        }
    } // namespace

    // ---- stream_decoder ----

    void
    stream_decoder::reset(std::string_view encoding,
                          std::optional<std::uint64_t> const& content_length,
                          std::uint64_t limit,
                          boost::system::error_code& ec)
    {
        ec = {};
        out_.clear();
        offset_ = 0;
        produced_ = 0;
        limit_ = limit;
        finished_ = false;
        compressor_.reset();

        compressor_ = make_compressor(encoding, compress::compressor::mode::decode, ec);
        if (ec || !compressor_)
        {
            return;
        }
        if (limit_ > 0 && content_length.has_value())
        {
            auto len = *content_length;
            if (len >= limit_)
            {
                ec = http::error::body_limit;
                compressor_.reset();
                return;
            }
            limit_ -= len;
        }
    }

    void
    stream_decoder::feed(net::const_buffer const& raw, boost::system::error_code& ec)
    {
        ec = {};
        if (compressor_)
        {
            compressor_->write(raw, true, ec);
            if (ec)
            {
                return;
            }
            pump(ec);
            return;
        }
        append(static_cast<char const*>(raw.data()), raw.size());
        account(raw.size(), ec);
    }

    std::size_t
    stream_decoder::drain(net::mutable_buffer const& dst, boost::system::error_code& ec)
    {
        ec = {};
        if (offset_ >= out_.size())
        {
            out_.clear();
            offset_ = 0;
            return 0;
        }
        auto const available = out_.size() - offset_;
        auto const n = (std::min)(dst.size(), available);
        std::memcpy(dst.data(), out_.data() + offset_, n);
        offset_ += n;
        if (offset_ == out_.size())
        {
            out_.clear();
            offset_ = 0;
        }
        return n;
    }

    void
    stream_decoder::flush(boost::system::error_code& ec)
    {
        ec = {};
        if (!compressor_ || finished_)
        {
            return;
        }
        finished_ = true;
        compressor_->finish(ec);
        if (ec)
        {
            return;
        }
        pump(ec);
    }

    void
    stream_decoder::pump(boost::system::error_code& ec)
    {
        auto buffer = compressor_->buffer();
        if (buffer.size() == 0)
        {
            return;
        }
        append(static_cast<char const*>(buffer.data()), buffer.size());
        compressor_->consume_all();
        account(buffer.size(), ec);
    }

    void
    stream_decoder::append(char const* data, std::size_t size)
    {
        if (offset_ > 0)
        {
            out_.erase(0, offset_);
            offset_ = 0;
        }
        out_.append(data, size);
    }

    void
    stream_decoder::account(std::size_t size, boost::system::error_code& ec)
    {
        if (limit_ > 0)
        {
            produced_ += size;
            if (produced_ > limit_)
            {
                ec = http::error::body_limit;
            }
        }
    }

    // ---- stream_encoder ----

    void
    stream_encoder::reset(std::string_view encoding, boost::system::error_code& ec)
    {
        ec = {};
        out_.clear();
        offset_ = 0;
        finished_ = false;
        compressor_.reset();
        compressor_ = make_compressor(encoding, compress::compressor::mode::encode, ec);
    }

    void
    stream_encoder::feed(net::const_buffer const& plain, bool more, boost::system::error_code& ec)
    {
        ec = {};
        if (!compressor_)
        {
            out_.append(static_cast<char const*>(plain.data()), plain.size());
            return;
        }
        compressor_->write(plain, more, ec);
        if (ec)
        {
            return;
        }
        pump(ec);
    }

    void
    stream_encoder::flush(boost::system::error_code& ec)
    {
        ec = {};
        if (!compressor_ || finished_)
        {
            return;
        }
        finished_ = true;
        compressor_->finish(ec);
        if (ec)
        {
            return;
        }
        pump(ec);
    }

    void
    stream_encoder::pump(boost::system::error_code& ec)
    {
        auto buffer = compressor_->buffer();
        if (buffer.size() == 0)
        {
            return;
        }
        if (offset_ > 0)
        {
            out_.erase(0, offset_);
            offset_ = 0;
        }
        out_.append(static_cast<char const*>(buffer.data()), buffer.size());
        compressor_->consume_all();
    }

    // ---- one-shot helpers ----

    boost::system::result<std::string>
    decode(std::string_view bytes, std::string_view encoding, std::uint64_t limit)
    {
        stream_decoder decoder;
        boost::system::error_code ec;
        decoder.reset(encoding, std::nullopt, limit, ec);
        if (ec)
        {
            return ec;
        }
        decoder.feed(net::buffer(bytes.data(), bytes.size()), ec);
        if (ec)
        {
            return ec;
        }
        decoder.flush(ec);
        if (ec)
        {
            return ec;
        }
        std::string out;
        out.resize(decoder.buffered());
        if (!out.empty())
        {
            auto const n = decoder.drain(net::buffer(out), ec);
            out.resize(n);
        }
        return out;
    }

    boost::system::result<std::string>
    encode(std::string_view bytes, std::string_view encoding)
    {
        stream_encoder encoder;
        boost::system::error_code ec;
        encoder.reset(encoding, ec);
        if (ec)
        {
            return ec;
        }
        if (!encoder.transforms())
        {
            return std::string(bytes);
        }
        encoder.feed(net::buffer(bytes.data(), bytes.size()), false, ec);
        if (ec)
        {
            return ec;
        }
        encoder.flush(ec);
        if (ec)
        {
            return ec;
        }
        auto buffer = encoder.buffer();
        return std::string(static_cast<char const*>(buffer.data()), buffer.size());
    }

} // namespace httplib::body
