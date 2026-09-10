#pragma once
#include "compress/compressor_error.hpp"
#include "httplib/util/string_hash.hpp"
#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>
#include <functional>
#include <unordered_map>

namespace httplib::compress
{
    class compressor
    {
      public:
        using ptr = std::unique_ptr<compressor>;

        enum class mode
        {
            encode,
            decode,
        };
        virtual ~compressor() = default;

        virtual void init(mode m, boost::system::error_code& ec) = 0;

        virtual net::const_buffer buffer() const = 0;
        virtual void write(net::const_buffer const& buffer, bool more, boost::system::error_code& ec) = 0;
        virtual void finish(boost::system::error_code& ec) = 0;
        virtual void consume_all() = 0;
        virtual void consume(std::size_t bytes) = 0;
    };

    class compressor_factory
    {
      public:
        using create_function = std::function<compressor::ptr()>;

        std::vector<std::string> const& supported_encoding() const;

        compressor::ptr create(std::string const& encoding);

        bool is_supported_encoding(std::string_view encoding) const;
        bool is_transform_encoding(std::string_view encoding) const;

      public:
        static compressor_factory& instance();

      private:
        struct entry
        {
            create_function create;
            bool transforms;
        };
        compressor_factory();
        void register_compressor(std::string const& encoding, create_function&& func, bool transforms = true);
        util::string_map<entry> creators_;
    };
} // namespace httplib::compress