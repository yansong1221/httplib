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

struct brotli_compressor_impl {
    typedef char char_type;

    brotli_compressor_impl(int quality)
        : encoder_state_(nullptr, &BrotliEncoderDestroyInstance)
    {
        encoder_state_.reset(BrotliEncoderCreateInstance(nullptr, nullptr, nullptr));
        if (!encoder_state_) {
            throw std::runtime_error("Failed to create Brotli encoder");
        }
        BrotliEncoderSetParameter(encoder_state_.get(), BROTLI_PARAM_QUALITY, quality);
    }

    bool
    filter(const char*& src_begin,
           const char* src_end,
           char*& dest_begin,
           char* dest_end,
           bool flush)
    {
        return true;
    }
    void
    close()
    {
    }

private:
    std::unique_ptr<BrotliEncoderState, void (*)(BrotliEncoderState*)> encoder_state_;
};


struct brotli_decompressor_impl {
    typedef char char_type;

    brotli_decompressor_impl()
        : decoder_state_(nullptr, &BrotliDecoderDestroyInstance), output_pos_(0)
    {
        decoder_state_.reset(BrotliDecoderCreateInstance(nullptr, nullptr, nullptr));
        if (!decoder_state_) {
            throw std::runtime_error("Failed to create Brotli decoder");
        }
    }

    bool
    filter(const char*& src_begin,
           const char* src_end,
           char*& dest_begin,
           char* dest_end,
           bool flush)
    {
        return true;
    }
    void
    close()
    {
    }

private:
    std::unique_ptr<BrotliDecoderState, void (*)(BrotliDecoderState*)> decoder_state_;
    size_t output_pos_;
};

} // namespace detail


template<typename Alloc = std::allocator<char>>
struct basic_brotli_compressor
    : boost::iostreams::symmetric_filter<detail::brotli_compressor_impl, Alloc> {
private:
    typedef detail::brotli_compressor_impl impl_type;
    typedef boost::iostreams::symmetric_filter<impl_type, Alloc> base_type;

public:
    typedef typename base_type::char_type char_type;
    typedef typename base_type::category category;
    basic_brotli_compressor(int quality                 = BROTLI_DEFAULT_QUALITY,
                            std::streamsize buffer_size = 4096)
        : base_type(buffer_size, quality)
    {
    }
};
BOOST_IOSTREAMS_PIPABLE(basic_brotli_compressor, 1)

typedef basic_brotli_compressor<> brotli_compressor;


template<typename Alloc = std::allocator<char>>
struct basic_brotli_decompressor
    : boost::iostreams::symmetric_filter<detail::brotli_decompressor_impl, Alloc> {
private:
    typedef detail::brotli_decompressor_impl impl_type;
    typedef boost::iostreams::symmetric_filter<impl_type, Alloc> base_type;

public:
    typedef typename base_type::char_type char_type;
    typedef typename base_type::category category;
    basic_brotli_decompressor(std::streamsize buffer_size = 4096) : base_type(buffer_size)
    {
    }
};
BOOST_IOSTREAMS_PIPABLE(basic_brotli_decompressor, 1)

typedef basic_brotli_compressor<> brotli_decompressor;

} // namespace httplib::body