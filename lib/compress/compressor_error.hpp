#pragma once
#include "compress/brotli_error.hpp"
#include "httplib/config.hpp"
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filter/zstd.hpp>
#include <boost/system/error_code.hpp>
#include <brotli/decode.h>
#include <exception>
#include <string>
#include <type_traits>
#include <zlib.h>
#include <zstd_errors.h>

// ============================================================================
// 异常→error_code 收敛层：底层编解码（boost::iostreams / brotli）以异常形式
// 报错，统一在此转化为 error_code，避免 C++ 异常逃出 Beast body reader/writer。
// 全部 inline 实现，header-only，多 TU 无 ODR 冲突。
// ============================================================================
namespace httplib::compress
{
    enum class error
    {
        encode_error = 1,      // 压缩/收尾失败
        decode_error = 2,      // 解码失败，无法归因到更细类
        bad_header = 3,        // 头无效或压缩方法不受支持
        bad_data = 4,          // 压缩数据流损坏（zlib data error）
        bad_checksum = 5,      // CRC-32 / 原始长度校验不符
        incomplete = 6,        // 流不完整 / 输入不足（含非 gzip 流的无进展情形）
        invalid_parameter = 7, // encode 专属：压缩参数/配置非法（级别、窗口等越界）
    };
    inline boost::system::error_code
    make_error_code(error e)
    {
        // error 枚举的自定义分类：message() 提供更可读的文本。
        struct category final : boost::system::error_category
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
                    case error::invalid_parameter:
                        return "compression stream invalid parameter";
                    default:
                        return "unknown compression error";
                }
            }
        };

        static category instance;
        return { static_cast<int>(e), instance };
    }

    namespace io = boost::iostreams;

    // gzip：gzip_error 携带 gzip 命名空间的细化码（gzip::*），内层 zlib 错误走 Z_*。
    inline boost::system::error_code
    map_gzip_error(io::gzip_error const& e)
    {
        switch (e.error())
        {
            case io::gzip::bad_crc:
                return make_error_code(error::bad_checksum);
            case io::gzip::bad_length:
                return make_error_code(error::incomplete);
            case io::gzip::bad_header:
            case io::gzip::bad_method:
                return make_error_code(error::bad_header);
            case io::gzip::zlib_error:
                switch (e.zlib_error_code())
                {
                    case Z_DATA_ERROR:
                        return make_error_code(error::bad_data);
                    case Z_BUF_ERROR:
                        return make_error_code(error::incomplete);
                    default:
                        return make_error_code(error::decode_error);
                }
            default:
                return make_error_code(error::decode_error);
        }
    }

    // zlib：使用 zlib.h 的 Z_* 错误码宏。
    inline boost::system::error_code
    map_zlib_error(io::zlib_error const& e)
    {
        switch (e.error())
        {
            case Z_DATA_ERROR:
                return make_error_code(error::bad_data);
            case Z_BUF_ERROR:
                return make_error_code(error::incomplete);
            case Z_STREAM_ERROR:
                return make_error_code(error::bad_data);
            default:
                return make_error_code(error::decode_error);
        }
    }

    // zstd：zstd_error::error() 即 ZSTD_ErrorCode 的负值，对照 zstd_errors.h。
    inline boost::system::error_code
    map_zstd_error(io::zstd_error const& e)
    {
        switch (e.error())
        {
            case -static_cast<int>(ZSTD_error_prefix_unknown):
            case -static_cast<int>(ZSTD_error_version_unsupported):
            case -static_cast<int>(ZSTD_error_frameParameter_unsupported):
            case -static_cast<int>(ZSTD_error_frameParameter_windowTooLarge):
                return make_error_code(error::bad_header);
            case -static_cast<int>(ZSTD_error_corruption_detected):
            case -static_cast<int>(ZSTD_error_literals_headerWrong):
            case -static_cast<int>(ZSTD_error_dictionary_corrupted):
                return make_error_code(error::bad_data);
            case -static_cast<int>(ZSTD_error_checksum_wrong):
                return make_error_code(error::bad_checksum);
            case -static_cast<int>(ZSTD_error_srcSize_wrong):
            case -static_cast<int>(ZSTD_error_noForwardProgress_destFull):
            case -static_cast<int>(ZSTD_error_noForwardProgress_inputEmpty):
                return make_error_code(error::incomplete);
            default:
                return make_error_code(error::decode_error);
        }
    }

    // brotli：brotli_error::error() 是抛点明确设定的 brotli 语义码。
    inline boost::system::error_code
    map_brotli_error(brotli_error const& e)
    {
        switch (e.error())
        {
            case brotli::bad_header:
                return make_error_code(error::bad_header);
            case brotli::bad_data:
                return make_error_code(error::bad_data);
            case brotli::incomplete:
                return make_error_code(error::incomplete);
            case brotli::invalid_parameter:
                return make_error_code(error::invalid_parameter);
            case brotli::encode_error:
            default:
                return make_error_code(error::encode_error);
        }
    }

    // decode 侧分发：rethrow exception_ptr 后按算法多 catch；算法异常无继承关系，
    // 顺序无关；末尾 catch(...) 兜底，任何异常都不外抛。
    inline boost::system::error_code
    map_decode_error(std::exception_ptr ep)
    {
        try
        {
            std::rethrow_exception(ep);
        }
        catch (io::gzip_error const& e)
        {
            return map_gzip_error(e);
        }
        catch (io::zlib_error const& e)
        {
            return map_zlib_error(e);
        }
        catch (io::zstd_error const& e)
        {
            return map_zstd_error(e);
        }
        catch (brotli_error const& e)
        {
            return map_brotli_error(e);
        }
        catch (...)
        {
            return make_error_code(error::decode_error);
        }
    }

    // encode 侧分发：参数/配置非法 → invalid_parameter，其余 → encode_error。
    inline boost::system::error_code
    map_encode_error(std::exception_ptr ep)
    {
        try
        {
            std::rethrow_exception(ep);
        }
        catch (io::gzip_error const& e)
        {
            // gzip 内层 zlib 错误：Z_STREAM_ERROR = 参数/状态非法。
            return e.error() == io::gzip::zlib_error && e.zlib_error_code() == Z_STREAM_ERROR
                       ? make_error_code(error::invalid_parameter)
                       : make_error_code(error::encode_error);
        }
        catch (io::zlib_error const& e)
        {
            return e.error() == Z_STREAM_ERROR ? make_error_code(error::invalid_parameter)
                                               : make_error_code(error::encode_error);
        }
        catch (io::zstd_error const& e)
        {
            switch (e.error())
            {
                case -static_cast<int>(ZSTD_error_parameter_unsupported):
                case -static_cast<int>(ZSTD_error_parameter_combination_unsupported):
                case -static_cast<int>(ZSTD_error_parameter_outOfBound):
                case -static_cast<int>(ZSTD_error_tableLog_tooLarge):
                case -static_cast<int>(ZSTD_error_maxSymbolValue_tooLarge):
                case -static_cast<int>(ZSTD_error_maxSymbolValue_tooSmall):
                case -static_cast<int>(ZSTD_error_stage_wrong):
                case -static_cast<int>(ZSTD_error_init_missing):
                    return make_error_code(error::invalid_parameter);
                default:
                    return make_error_code(error::encode_error);
            }
        }
        catch (brotli_error const& e)
        {
            return map_brotli_error(e);
        }
        catch (...)
        {
            return make_error_code(error::encode_error);
        }
    }
} // namespace httplib::compress

namespace boost::system
{
    template <>
    struct is_error_code_enum<httplib::compress::error> : std::true_type
    {
    };
} // namespace boost::system