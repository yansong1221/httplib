#pragma once
#include "httplib/config.hpp"
#include "httplib/util/string_hash.hpp"
#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>
#include <functional>
#include <unordered_map>

namespace httplib::compress
{
    // 底层编解码失败（boost::iostreams / brotli 等以异常形式抛出的错误）统一
    // 转换为本 error_code，避免 C++ 异常逃出 Beast body reader/writer。
    enum class error
    {
        encode_error = 1, // 压缩/收尾失败
        decode_error = 2, // 解码失败，无法归因到更细类
        bad_header = 3,   // 头无效或压缩方法不受支持
        bad_data = 4,     // 压缩数据流损坏（zlib data error）
        bad_checksum = 5, // CRC-32 / 原始长度校验不符
        incomplete = 6,   // 流不完整 / 输入不足（含非 gzip 流的无进展情形）
    };

    HTTPLIB_API boost::system::error_code make_error_code(error e);

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
        // 以下操作均可能因底层编解码失败而报错，统一通过 ec 上报，不抛 C++ 异常。
        virtual void init(mode m, boost::system::error_code& ec) = 0;

        virtual net::const_buffer buffer() const = 0;
        virtual void write(net::const_buffer const& buffer, bool more, boost::system::error_code& ec) = 0;
        virtual void finish(boost::system::error_code& ec) = 0;
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

namespace boost::system
{
    template <>
    struct is_error_code_enum<httplib::compress::error> : std::true_type
    {
    };
} // namespace boost::system