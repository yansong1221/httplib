#pragma once
#include "compress/compressor_error.hpp"
#include <boost/iostreams/concepts.hpp>
#include <boost/iostreams/detail/ios.hpp>
#include <boost/iostreams/filter/symmetric.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/operations.hpp>
#include <brotli/decode.h>
#include <brotli/encode.h>
#include <memory>
#include <stdexcept>
#include <vector>

namespace httplib::compress
{
    class brotli_error : public BOOST_IOSTREAMS_FAILURE
    {
      public:
        explicit brotli_error(error code, std::string const& what = "Brotli decompression error")
            : BOOST_IOSTREAMS_FAILURE(what)
            , code_(code)
            , detail_(0)
        {
        }

        brotli_error(error code, int detail, std::string const& what = "Brotli decompression error")
            : BOOST_IOSTREAMS_FAILURE(what)
            , code_(code)
            , detail_(detail)
        {
        }

        error
        code() const noexcept
        {
            return code_;
        }

        int
        detail() const noexcept
        {
            return detail_;
        }

      private:
        error code_;
        int detail_;
    };

    namespace detail
    {

        struct brotli_compressor_impl
        {
            typedef char char_type;

            brotli_compressor_impl(int quality) : state_(nullptr, &BrotliEncoderDestroyInstance)
            {
                state_.reset(BrotliEncoderCreateInstance(nullptr, nullptr, nullptr));
                if (!state_)
                {
                    throw brotli_error(error::encode_error, "Failed to create Brotli encoder");
                }
                if (!BrotliEncoderSetParameter(state_.get(), BROTLI_PARAM_QUALITY, static_cast<uint32_t>(quality)))
                {
                    throw brotli_error(error::invalid_parameter, "Invalid Brotli encoder parameter");
                }
            }

            bool
            filter(char const*& src_begin, char const* src_end, char*& dst_begin, char* dst_end, bool flush)
            {
                uint8_t const* next_in = reinterpret_cast<uint8_t const*>(src_begin);
                size_t available_in = src_end - src_begin;
                uint8_t* next_out = reinterpret_cast<uint8_t*>(dst_begin);
                size_t available_out = dst_end - dst_begin;

                BrotliEncoderOperation op = flush ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS;

                if (!BrotliEncoderCompressStream(state_.get(),
                                                 op,
                                                 &available_in,
                                                 &next_in,
                                                 &available_out,
                                                 &next_out,
                                                 nullptr))
                {
                    throw brotli_error(error::encode_error, "Brotli compression failed");
                }

                src_begin = reinterpret_cast<char const*>(next_in);
                dst_begin = reinterpret_cast<char*>(next_out);

                return BrotliEncoderHasMoreOutput(state_.get());
            }
            void
            close()
            {
                if (BrotliEncoderIsFinished(state_.get()))
                {
                    return;
                }

                uint8_t const* next_in = nullptr;
                size_t available_in = 0;
                uint8_t* next_out = nullptr;
                size_t available_out = 0;

                while (!BrotliEncoderIsFinished(state_.get()))
                {
                    if (!BrotliEncoderCompressStream(state_.get(),
                                                     BROTLI_OPERATION_FINISH,
                                                     &available_in,
                                                     &next_in,
                                                     &available_out,
                                                     &next_out,
                                                     nullptr))
                    {
                        throw brotli_error(error::encode_error, "Brotli finalization failed");
                    }
                }
            }

          private:
            std::unique_ptr<BrotliEncoderState, void (*)(BrotliEncoderState*)> state_;
        };

        struct brotli_decompressor_impl
        {
            typedef char char_type;

            brotli_decompressor_impl() : state_(nullptr, &BrotliDecoderDestroyInstance)
            {
                state_.reset(BrotliDecoderCreateInstance(nullptr, nullptr, nullptr));
                if (!state_)
                {
                    throw std::runtime_error("Failed to create Brotli decoder");
                }
            }

            bool
            filter(char const*& src_begin, char const* src_end, char*& dst_begin, char* dst_end, bool flush)
            {
                uint8_t const* next_in = reinterpret_cast<uint8_t const*>(src_begin);
                size_t available_in = src_end - src_begin;
                char* out_start = dst_begin;
                uint8_t* next_out = reinterpret_cast<uint8_t*>(dst_begin);
                size_t available_out = dst_end - dst_begin;

                BrotliDecoderResult result = BrotliDecoderDecompressStream(state_.get(),
                                                                           &available_in,
                                                                           &next_in,
                                                                           &available_out,
                                                                           &next_out,
                                                                           nullptr);

                src_begin = reinterpret_cast<char const*>(next_in);
                dst_begin = reinterpret_cast<char*>(next_out);
                decoded_total_ += static_cast<size_t>(next_out - reinterpret_cast<uint8_t*>(out_start));

                switch (result)
                {
                    case BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT:
                        return false; // 需要更多输入数据
                    case BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT:
                        return true; // 需要更多输出空间
                    case BROTLI_DECODER_RESULT_SUCCESS:
                        return false; // 解压完成
                    case BROTLI_DECODER_RESULT_ERROR:
                        throw brotli_error(classify_error(),
                                           BrotliDecoderGetErrorCode(state_.get()),
                                           "Brotli decompression error");
                    default:
                        throw brotli_error(classify_error(), "Unknown Brotli result");
                }
            }
            void
            close()
            {
                BrotliDecoderErrorCode code = BrotliDecoderGetErrorCode(state_.get());
                if (code == BROTLI_DECODER_NEEDS_MORE_INPUT)
                {
                    throw brotli_error(error::incomplete, "Brotli stream truncated");
                }
                if (code < 0)
                {
                    throw brotli_error(classify_error(), code, "Brotli decompression error");
                }
            }

          private:
            error
            classify_error() const noexcept
            {
                return decoded_total_ == 0 ? error::bad_header : error::bad_data;
            }

            std::unique_ptr<BrotliDecoderState, void (*)(BrotliDecoderState*)> state_;
            std::size_t decoded_total_ = 0;
        };

    } // namespace detail

    template <typename Alloc = std::allocator<char>>
    struct basic_brotli_compressor : boost::iostreams::symmetric_filter<detail::brotli_compressor_impl, Alloc>
    {
      private:
        typedef detail::brotli_compressor_impl impl_type;
        typedef boost::iostreams::symmetric_filter<impl_type, Alloc> base_type;

      public:
        typedef typename base_type::char_type char_type;
        typedef typename base_type::category category;
        basic_brotli_compressor(int quality = 6, std::streamsize buffer_size = 4096) : base_type(buffer_size, quality)
        {
        }
    };
    BOOST_IOSTREAMS_PIPABLE(basic_brotli_compressor, 1)

    typedef basic_brotli_compressor<> brotli_compressor;

    template <typename Alloc = std::allocator<char>>
    struct basic_brotli_decompressor : boost::iostreams::symmetric_filter<detail::brotli_decompressor_impl, Alloc>
    {
      private:
        typedef detail::brotli_decompressor_impl impl_type;
        typedef boost::iostreams::symmetric_filter<impl_type, Alloc> base_type;

      public:
        typedef typename base_type::char_type char_type;
        typedef typename base_type::category category;
        basic_brotli_decompressor(std::streamsize buffer_size = 4096) : base_type(buffer_size) {}
    };
    BOOST_IOSTREAMS_PIPABLE(basic_brotli_decompressor, 1)

    typedef basic_brotli_decompressor<> brotli_decompressor;

} // namespace httplib::compress