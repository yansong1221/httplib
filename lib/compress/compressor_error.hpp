#pragma once
#include "httplib/config.hpp"
#include <boost/system/error_code.hpp>
#include <exception>
#include <type_traits>

namespace httplib::compress
{
    // 底层编解码失败（boost::iostreams / brotli 等以异常形式抛出的错误）统一
    // 转换为本 error_code，避免 C++ 异常逃出 Beast body reader/writer。
    enum class error
    {
        encode_error = 1,   // 压缩/收尾失败
        decode_error = 2,   // 解码失败，无法归因到更细类
        bad_header = 3,     // 头无效或压缩方法不受支持
        bad_data = 4,       // 压缩数据流损坏（zlib data error）
        bad_checksum = 5,   // CRC-32 / 原始长度校验不符
        incomplete = 6,     // 流不完整 / 输入不足（含非 gzip 流的无进展情形）
        invalid_parameter = 7, // encode 专属：压缩参数/配置非法（级别、窗口等越界）
    };

    boost::system::error_code make_error_code(error e);

    // 底层异常 → error_code 的收敛入口，供 compressor 实现（compressor.cpp）调用。
    // 经 exception_ptr 保留原始类型，用多 catch 按算法分发，对任何异常都绝不外抛。
    boost::system::error_code map_decode_error(std::exception_ptr ep);
    boost::system::error_code map_encode_error(std::exception_ptr ep);
} // namespace httplib::compress

namespace boost::system
{
    template <>
    struct is_error_code_enum<httplib::compress::error> : std::true_type
    {
    };
} // namespace boost::system