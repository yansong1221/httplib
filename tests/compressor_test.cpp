#include "compress/compressor.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

TEST_CASE("Compressor: factory instance is singleton", "[compressor]")
{
    auto& f1 = httplib::compress::compressor_factory::instance();
    auto& f2 = httplib::compress::compressor_factory::instance();
    REQUIRE(&f1 == &f2);
}

TEST_CASE("Compressor: unsupported encoding returns null", "[compressor]")
{
    auto& factory = httplib::compress::compressor_factory::instance();
    REQUIRE_FALSE(factory.is_supported_encoding("unknown-encoding"));
    REQUIRE(factory.create("unknown-encoding") == nullptr);
}

TEST_CASE("Compressor: supported_encoding returns known list", "[compressor]")
{
    auto& factory = httplib::compress::compressor_factory::instance();
    auto encodings = factory.supported_encoding();
    REQUIRE_FALSE(encodings.empty());
}

#ifdef HTTPLIB_ENABLED_COMPRESS
TEST_CASE("Compressor: gzip encode/decode roundtrip", "[compressor]")
{
    auto& factory = httplib::compress::compressor_factory::instance();
    REQUIRE(factory.is_supported_encoding("gzip"));

    std::string original = "hello world from httplib compressor test!";

    auto encoder = factory.create("gzip");
    REQUIRE(encoder != nullptr);
    encoder->init(httplib::compress::compressor::mode::encode);
    encoder->write(boost::asio::buffer(original), false);
    auto compressed_buf = encoder->buffer();
    std::string compressed(static_cast<char const*>(compressed_buf.data()), compressed_buf.size());
    REQUIRE_FALSE(compressed.empty());

    auto decoder = factory.create("gzip");
    REQUIRE(decoder != nullptr);
    decoder->init(httplib::compress::compressor::mode::decode);
    decoder->write(boost::asio::buffer(compressed), false);
    auto decompressed_buf = decoder->buffer();
    std::string decompressed(static_cast<char const*>(decompressed_buf.data()), decompressed_buf.size());

    REQUIRE(decompressed == original);
}
#endif
