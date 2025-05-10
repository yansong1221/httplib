#pragma once
#include <boost/iostreams/concepts.hpp>
#include <boost/iostreams/filter/symmetric.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/operations.hpp>
#include <brotli/decode.h>
#include <brotli/encode.h>
#include <memory>
#include <stdexcept>
#include <vector>

namespace httplib::body {


namespace detail {

struct brotli_compressor_impl
{
    typedef char char_type;

    brotli_compressor_impl(int quality)
        : state_(nullptr, &BrotliEncoderDestroyInstance)
    {
        state_.reset(BrotliEncoderCreateInstance(nullptr, nullptr, nullptr));
        if (!state_) {
            throw std::runtime_error("Failed to create Brotli encoder");
        }
        BrotliEncoderSetParameter(state_.get(), BROTLI_PARAM_QUALITY, quality);
    }

    bool
    filter(const char*& src_begin, const char* src_end, char*& dst_begin, char* dst_end, bool flush)
    {
        const uint8_t* next_in = reinterpret_cast<const uint8_t*>(src_begin);
        size_t available_in    = src_end - src_begin;
        uint8_t* next_out      = reinterpret_cast<uint8_t*>(dst_begin);
        size_t available_out   = dst_end - dst_begin;

        BrotliEncoderOperation op = flush ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS;

        if (!BrotliEncoderCompressStream(
                state_.get(), op, &available_in, &next_in, &available_out, &next_out, nullptr))
        {
            throw std::runtime_error("Brotli compression failed");
        }

        src_begin = reinterpret_cast<const char*>(next_in);
        dst_begin = reinterpret_cast<char*>(next_out);

        // 返回 true 表示需要更多输出空间
        return BrotliEncoderHasMoreOutput(state_.get());
    }
    void close()
    { // 确保所有数据已刷新
        if (BrotliEncoderIsFinished(state_.get()))
            return;

        const uint8_t* next_in = nullptr;
        size_t available_in    = 0;
        uint8_t* next_out      = nullptr;
        size_t available_out   = 0;

        while (!BrotliEncoderIsFinished(state_.get())) {
            if (!BrotliEncoderCompressStream(state_.get(),
                                             BROTLI_OPERATION_FINISH,
                                             &available_in,
                                             &next_in,
                                             &available_out,
                                             &next_out,
                                             nullptr))
            {
                throw std::runtime_error("Brotli finalization failed");
            }
        }
    }

private:
    std::unique_ptr<BrotliEncoderState, void (*)(BrotliEncoderState*)> state_;
};


struct brotli_decompressor_impl
{
    typedef char char_type;

    brotli_decompressor_impl()
        : state_(nullptr, &BrotliDecoderDestroyInstance)
    {
        state_.reset(BrotliDecoderCreateInstance(nullptr, nullptr, nullptr));
        if (!state_) {
            throw std::runtime_error("Failed to create Brotli decoder");
        }
    }

    bool
    filter(const char*& src_begin, const char* src_end, char*& dst_begin, char* dst_end, bool flush)
    {
        const uint8_t* next_in = reinterpret_cast<const uint8_t*>(src_begin);
        size_t available_in    = src_end - src_begin;
        uint8_t* next_out      = reinterpret_cast<uint8_t*>(dst_begin);
        size_t available_out   = dst_end - dst_begin;

        BrotliDecoderResult result = BrotliDecoderDecompressStream(
            state_.get(), &available_in, &next_in, &available_out, &next_out, nullptr);

        src_begin = reinterpret_cast<const char*>(next_in);
        dst_begin = reinterpret_cast<char*>(next_out);

        switch (result) {
            case BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT: return false; // 需要更多输入数据
            case BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT: return true; // 需要更多输出空间
            case BROTLI_DECODER_RESULT_SUCCESS: return false;          // 解压完成
            case BROTLI_DECODER_RESULT_ERROR:
                throw std::runtime_error("Brotli decompression error");
            default: throw std::runtime_error("Unknown Brotli result");
        }
    }
    void close()
    { // 确保状态清理
        BrotliDecoderErrorCode code = BrotliDecoderGetErrorCode(state_.get());
        if (code != BROTLI_DECODER_RESULT_SUCCESS) {
            throw std::runtime_error("Brotli decompression closed with error");
        }
    }

private:
    std::unique_ptr<BrotliDecoderState, void (*)(BrotliDecoderState*)> state_;
};

} // namespace detail


template<typename Alloc = std::allocator<char>>
struct basic_brotli_compressor
    : boost::iostreams::symmetric_filter<detail::brotli_compressor_impl, Alloc>
{
private:
    typedef detail::brotli_compressor_impl impl_type;
    typedef boost::iostreams::symmetric_filter<impl_type, Alloc> base_type;

public:
    typedef typename base_type::char_type char_type;
    typedef typename base_type::category category;
    basic_brotli_compressor(int quality = 6, std::streamsize buffer_size = 4096)
        : base_type(buffer_size, quality)
    {
    }
};
BOOST_IOSTREAMS_PIPABLE(basic_brotli_compressor, 1)

typedef basic_brotli_compressor<> brotli_compressor;


template<typename Alloc = std::allocator<char>>
struct basic_brotli_decompressor
    : boost::iostreams::symmetric_filter<detail::brotli_decompressor_impl, Alloc>
{
private:
    typedef detail::brotli_decompressor_impl impl_type;
    typedef boost::iostreams::symmetric_filter<impl_type, Alloc> base_type;

public:
    typedef typename base_type::char_type char_type;
    typedef typename base_type::category category;
    basic_brotli_decompressor(std::streamsize buffer_size = 4096)
        : base_type(buffer_size)
    {
    }
};
BOOST_IOSTREAMS_PIPABLE(basic_brotli_decompressor, 1)

typedef basic_brotli_compressor<> brotli_decompressor;

} // namespace httplib::body