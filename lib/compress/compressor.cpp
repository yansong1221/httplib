#include "compress/compressor.hpp"

#ifdef HTTPLIB_ENABLED_COMPRESS
#include "compress/brotli.hpp"
#include <boost/asio/streambuf.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filter/zstd.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#endif

namespace httplib::compress
{
    namespace
    {
        struct compressor_error_category : boost::system::error_category
        {
            char const*
            name() const noexcept override
            {
                return "httplib.compress";
            }

            std::string
            message(int ev) const override
            {
                switch (static_cast<error>(ev))
                {
                    case error::encode_error:
                        return "compression stream encode failure";
                    case error::decode_error:
                        return "compression stream decode failure";
                    case error::bad_header:
                        return "compression stream invalid or unsupported header";
                    case error::bad_data:
                        return "compression stream data corrupted";
                    case error::bad_checksum:
                        return "compression stream checksum mismatch";
                    case error::incomplete:
                        return "compression stream truncated or insufficient input";
                    default:
                        return "unknown compression error";
                }
            }
        };
    } // namespace

    boost::system::error_code
    make_error_code(error e)
    {
        static compressor_error_category category;
        return { static_cast<int>(e), category };
    }

#ifdef HTTPLIB_ENABLED_COMPRESS
    namespace io = boost::iostreams;

    // 把解码异常映射到细分错误码：gzip_error / zlib_error / zstd_error 携带各自的
    // 细分码；brotli 为自家实现，直接抛携带 error 枚举的 brotli_error。
    // 数值取自 zlib/gzip 错误码（稳定）：gzip: 1=zlib 错误, 2=bad_crc, 3=bad_length,
    // 4=bad_header, 6=bad_method；zlib: -2=stream, -3=data, -5=buf。
    boost::system::error_code
    map_decode_error(std::exception const& e)
    {
        auto const decode_generic = make_error_code(error::decode_error);
        if (auto* gzip_e = dynamic_cast<io::gzip_error const*>(&e))
        {
            switch (gzip_e->error())
            {
                case 2: // gzip::bad_crc
                    return make_error_code(error::bad_checksum);
                case 3: // gzip::bad_length
                    return make_error_code(error::incomplete);
                case 4: // gzip::bad_header
                case 6: // gzip::bad_method
                    return make_error_code(error::bad_header);
                case 1: // gzip::zlib_error，具体原因在 zlib_error_code()
                    switch (gzip_e->zlib_error_code())
                    {
                        case -3: // Z_DATA_ERROR
                            return make_error_code(error::bad_data);
                        case -5: // Z_BUF_ERROR
                            return make_error_code(error::incomplete);
                        default:
                            return decode_generic;
                    }
                default:
                    return decode_generic;
            }
        }
        if (auto* zlib_e = dynamic_cast<io::zlib_error const*>(&e))
        {
            switch (zlib_e->error())
            {
                case -3: // Z_DATA_ERROR
                    return make_error_code(error::bad_data);
                case -5: // Z_BUF_ERROR
                    return make_error_code(error::incomplete);
                case -2: // Z_STREAM_ERROR
                    return make_error_code(error::bad_data);
                default:
                    return decode_generic;
            }
        }
        if (auto* zstd_e = dynamic_cast<io::zstd_error const*>(&e))
        {
            // zstd_error::error() 即 ZSTD_ErrorCode 的负值（-prefix_unknown、-corruption_detected 等）。
            switch (zstd_e->error())
            {
                case -10: // prefix_unknown（魔数错误）
                case -12: // version_unsupported
                case -14: // frameParameter_unsupported
                case -16: // frameParameter_windowTooLarge
                    return make_error_code(error::bad_header);
                case -20: // corruption_detected
                case -24: // literals_headerWrong
                case -30: // dictionary_corrupted
                    return make_error_code(error::bad_data);
                case -22: // checksum_wrong
                    return make_error_code(error::bad_checksum);
                case -72: // srcSize_wrong
                case -80: // noForwardProgress_destFull
                case -82: // noForwardProgress_inputEmpty
                    return make_error_code(error::incomplete);
                default:
                    return decode_generic;
            }
        }
        if (auto* br_e = dynamic_cast<brotli_error const*>(&e))
        {
            return make_error_code(br_e->code());
        }
        return decode_generic;
    }

    class basic_compressor : public compressor
    {
      public:
        explicit basic_compressor() {}

        void
        init(mode m, boost::system::error_code& ec) override
        {
            mode_ = m;
            try
            {
                init_filtering_ostreambuf(m, stream_);
                stream_.push(buffer_);
            }
            catch (std::exception const& e)
            {
                ec = m == mode::decode ? map_decode_error(e) : make_error_code(error::encode_error);
            }
        }

        net::const_buffer
        buffer() const
        {
            return buffer_.data();
        }
        void
        write(net::const_buffer const& buffer, bool more, boost::system::error_code& ec) override
        {
            try
            {
                io::write(stream_, (char const*)buffer.data(), buffer.size());
                if (!more)
                {
                    io::close(stream_);
                }
            }
            catch (std::exception const& e)
            {
                ec = mode_ == mode::decode ? map_decode_error(e) : make_error_code(error::encode_error);
            }
        }
        void
        finish(boost::system::error_code& ec) override
        {
            try
            {
                io::close(stream_);
            }
            catch (std::exception const& e)
            {
                ec = mode_ == mode::decode ? map_decode_error(e) : make_error_code(error::encode_error);
            }
        }
        void
        consume_all()
        {
            buffer_.consume(buffer_.size());
        }
        void
        consume(std::size_t bytes)
        {
            buffer_.consume(bytes);
        }

      protected:
        virtual void init_filtering_ostreambuf(mode m, io::filtering_ostreambuf& stream) = 0;

      private:
        mode mode_ = mode::decode;
        net::streambuf buffer_;
        io::filtering_ostreambuf stream_;
    };

    class gzip_compressor_adapter : public basic_compressor
    {
      protected:
        void
        init_filtering_ostreambuf(mode m, io::filtering_ostreambuf& stream) override
        {
            switch (m)
            {
                case mode::encode:
                    stream.push(io::gzip_compressor());
                    break;
                case mode::decode:
                    stream.push(io::gzip_decompressor());
                    break;
                default:
                    break;
            }
        }
    };
    class zlib_compressor_adapter : public basic_compressor
    {
      protected:
        void
        init_filtering_ostreambuf(mode m, io::filtering_ostreambuf& stream) override
        {
            switch (m)
            {
                case mode::encode:
                    stream.push(io::zlib_compressor());
                    break;
                case mode::decode:
                    stream.push(io::zlib_decompressor());
                    break;
                default:
                    break;
            }
        }
    };
    class zstd_compressor_adapter : public basic_compressor
    {
      protected:
        void
        init_filtering_ostreambuf(mode m, io::filtering_ostreambuf& stream) override
        {
            switch (m)
            {
                case mode::encode:
                    stream.push(io::zstd_compressor());
                    break;
                case mode::decode:
                    stream.push(io::zstd_decompressor());
                    break;
                default:
                    break;
            }
        }
    };
    class brotli_compressor_adapter : public basic_compressor
    {
      protected:
        void
        init_filtering_ostreambuf(mode m, io::filtering_ostreambuf& stream) override
        {
            switch (m)
            {
                case mode::encode:
                    stream.push(brotli_compressor());
                    break;
                case mode::decode:
                    stream.push(brotli_decompressor());
                    break;
                default:
                    break;
            }
        }
    };

#endif

    compressor_factory::compressor_factory()
    {
        register_compressor("identity", []() { return nullptr; }, false);
#ifdef HTTPLIB_ENABLED_COMPRESS
        register_compressor("gzip", []() { return std::make_unique<gzip_compressor_adapter>(); });
        register_compressor("deflate", []() { return std::make_unique<zlib_compressor_adapter>(); });
        register_compressor("zstd", []() { return std::make_unique<zstd_compressor_adapter>(); });
        register_compressor("br", []() { return std::make_unique<brotli_compressor_adapter>(); });
#endif
    }
    compressor_factory&
    compressor_factory::instance()
    {
        static compressor_factory _instance;
        return _instance;
    }

    std::vector<std::string> const&
    compressor_factory::supported_encoding() const
    {
        static std::vector<std::string> result = [this]()
        {
            std::vector<std::string> result;
            for (auto const& v : creators_)
            {
                result.push_back(v.first);
            }
            return result;
        }();
        return result;
    }

    void
    compressor_factory::register_compressor(std::string const& encoding, create_function&& func, bool transforms)
    {
        creators_[encoding] = entry { std::move(func), transforms };
    }

    compressor::ptr
    compressor_factory::create(std::string const& encoding)
    {
        auto iter = creators_.find(encoding);
        if (iter == creators_.end())
        {
            return nullptr;
        }
        return iter->second.create();
    }

    bool
    compressor_factory::is_supported_encoding(std::string_view encoding) const
    {
        auto iter = creators_.find(encoding);
        return iter != creators_.end();
    }

    bool
    compressor_factory::is_transform_encoding(std::string_view encoding) const
    {
        auto iter = creators_.find(encoding);
        return iter != creators_.end() && iter->second.transforms;
    }

} // namespace httplib::compress