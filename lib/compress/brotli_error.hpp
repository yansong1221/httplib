#pragma once
#include <boost/iostreams/detail/ios.hpp>
#include <string>

namespace httplib::compress
{
    // brotli 语义错误码层，设计对齐 boost::iostreams 的 gzip namespace：
    // error() 携带本层的语义码（抛点明确设定，不必依赖 BrotliDecoderErrorCode），
    // detail() 保留底层 BrotliDecoderErrorCode（见 brotli/decode.h），无则为 0。
    // 转换为框架 error_code 见 compressor_error.hpp 的 map_brotli_error。
    namespace brotli
    {
        inline constexpr int bad_header = 1;        // 解压起始即失败：头部/元数据格式非法
        inline constexpr int bad_data = 2;          // 解压过程中数据损坏
        inline constexpr int incomplete = 3;        // 流被截断
        inline constexpr int encode_error = 4;      // 编码失败（底层库不提供错误码）
        inline constexpr int invalid_parameter = 5; // 编码参数非法
    } // namespace brotli

    class brotli_error : public BOOST_IOSTREAMS_FAILURE
    {
      public:
        explicit brotli_error(int error) : BOOST_IOSTREAMS_FAILURE("brotli error"), error_(error), detail_(0) {}
        explicit brotli_error(int error, int detail)
            : BOOST_IOSTREAMS_FAILURE("brotli error")
            , error_(error)
            , detail_(detail)
        {
        }
        int
        error() const noexcept
        {
            return error_;
        }
        int
        detail() const noexcept
        {
            return detail_;
        }

      private:
        int error_;
        int detail_;
    };
} // namespace httplib::compress