#include "compress/compressor_error.hpp"

#ifdef HTTPLIB_ENABLED_COMPRESS
#include "compress/brotli.hpp"
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filter/zstd.hpp>
#include <zlib.h>
#include <zstd_errors.h>
#endif

namespace httplib::compress
{
    namespace io = boost::iostreams;

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
                case error::invalid_parameter:
                    return "compression stream invalid parameter";
                default:
                    return "unknown compression error";
            }
        }
    };
    boost::system::error_code
    make_error_code(error e)
    {
        static compressor_error_category category;
        return { static_cast<int>(e), category };
    }

    namespace detail
    {

#ifdef HTTPLIB_ENABLED_COMPRESS

        static boost::system::error_code
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

        static boost::system::error_code
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

        static boost::system::error_code
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

        static boost::system::error_code
        map_brotli_error(brotli_error const& e)
        {
            return make_error_code(e.code());
        }
#endif
    } // namespace detail

    boost::system::error_code
    map_decode_error(std::exception_ptr ep)
    {
        try
        {
            std::rethrow_exception(ep);
        }
#ifdef HTTPLIB_ENABLED_COMPRESS
        catch (io::gzip_error const& e)
        {
            return detail::map_gzip_error(e);
        }
        catch (io::zlib_error const& e)
        {
            return detail::map_zlib_error(e);
        }
        catch (io::zstd_error const& e)
        {
            return detail::map_zstd_error(e);
        }
        catch (brotli_error const& e)
        {
            return detail::map_brotli_error(e);
        }
#endif
        catch (...)
        {
            return make_error_code(error::decode_error);
        }
    }
    boost::system::error_code
    map_encode_error(std::exception_ptr ep)
    {
        try
        {
            std::rethrow_exception(ep);
        }
#ifdef HTTPLIB_ENABLED_COMPRESS
        catch (io::gzip_error const& e)
        {
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
            return make_error_code(e.code());
        }
#endif
        catch (...)
        {
            return make_error_code(error::encode_error);
        }
    }

} // namespace httplib::compress