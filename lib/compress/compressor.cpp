#include "compress/compressor.hpp"

#ifdef HTTPLIB_ENABLED_COMPRESS
#    include "compress/brotli.hpp"
#    include <boost/asio/streambuf.hpp>
#    include <boost/iostreams/filter/gzip.hpp>
#    include <boost/iostreams/filter/zlib.hpp>
#    include <boost/iostreams/filter/zstd.hpp>
#    include <boost/iostreams/filtering_stream.hpp>
#endif

namespace httplib::compress
{
#ifdef HTTPLIB_ENABLED_COMPRESS
    namespace io = boost::iostreams;

    class basic_compressor : public compressor
    {
      public:
        explicit basic_compressor() {}

        void
        init(mode m)
        {
            init_filtering_ostreambuf(m, stream_);
            stream_.push(buffer_);
        }

        net::const_buffer
        buffer() const
        {
            return buffer_.data();
        }
        void
        write(net::const_buffer const& buffer, bool more = true)
        {
            io::write(stream_, (char const*)buffer.data(), buffer.size());
            if (!more)
            {
                io::close(stream_);
            }
        }
        void
        finish()
        {
            io::close(stream_);
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
        register_compressor("identity", []() { return nullptr; });
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
    compressor_factory::register_compressor(std::string const& encoding, create_function&& func)
    {
        creators_[encoding] = std::move(func);
    }

    compressor::ptr
    compressor_factory::create(std::string const& encoding)
    {
        auto iter = creators_.find(encoding);
        if (iter == creators_.end())
        {
            return nullptr;
        }
        return iter->second();
    }

    bool
    compressor_factory::is_supported_encoding(std::string_view encoding) const
    {
        auto iter = creators_.find(std::string(encoding));
        return iter != creators_.end();
    }

} // namespace httplib::compress