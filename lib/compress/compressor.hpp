#pragma once
#include "httplib/config.hpp"
#include "httplib/util/string_hash.hpp"
#include <boost/asio/buffer.hpp>
#include <functional>
#include <unordered_map>

namespace httplib::compress
{
    class HTTPLIB_API compressor
    {
      public:
        using ptr = std::unique_ptr<compressor>;

        enum class mode
        {
            encode,
            decode,
        };
        virtual ~compressor() = default;
        virtual void init(mode m) = 0;

        virtual net::const_buffer buffer() const = 0;
        virtual void write(net::const_buffer const& buffer, bool more = true) = 0;
        virtual void finish() = 0;
        virtual void consume_all() = 0;
        virtual void consume(std::size_t bytes) = 0;
    };

    class HTTPLIB_API compressor_factory
    {
      public:
        using create_function = std::function<compressor::ptr()>;

        std::vector<std::string> const& supported_encoding() const;

        compressor::ptr create(std::string const& encoding);

        bool is_supported_encoding(std::string_view encoding) const;

        // 该编码是否会真实变换 body（create() 会返回非空实现）。
        // 与 is_supported_encoding() 的区别：identity 等 no-op 编码虽被识别，但不会变换数据。
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