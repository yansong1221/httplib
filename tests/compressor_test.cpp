#include "body/any_body.hpp"
#include "compress/compressor.hpp"
#include <boost/asio/buffer.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

namespace body = httplib::body;
namespace http = httplib::http;

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
    boost::system::error_code ec;
    encoder->init(httplib::compress::compressor::mode::encode, ec);
    REQUIRE_FALSE(ec);
    encoder->write(boost::asio::buffer(original), false, ec);
    REQUIRE_FALSE(ec);
    auto compressed_buf = encoder->buffer();
    std::string compressed(static_cast<char const*>(compressed_buf.data()), compressed_buf.size());
    REQUIRE_FALSE(compressed.empty());

    auto decoder = factory.create("gzip");
    REQUIRE(decoder != nullptr);
    decoder->init(httplib::compress::compressor::mode::decode, ec);
    REQUIRE_FALSE(ec);
    decoder->write(boost::asio::buffer(compressed), false, ec);
    REQUIRE_FALSE(ec);
    auto decompressed_buf = decoder->buffer();
    std::string decompressed(static_cast<char const*>(decompressed_buf.data()), decompressed_buf.size());

    REQUIRE(decompressed == original);
}

namespace
{
    std::string encode(std::string_view encoding, std::string const& data);

    std::string
    gzip_encode(std::string const& data)
    {
        return encode("gzip", data);
    }

    std::string
    encode(std::string_view encoding, std::string const& data)
    {
        auto& factory = httplib::compress::compressor_factory::instance();
        boost::system::error_code ec;
        auto encoder = factory.create(std::string(encoding));
        if (!encoder)
        {
            return {};
        }
        encoder->init(httplib::compress::compressor::mode::encode, ec);
        REQUIRE_FALSE(ec);
        encoder->write(boost::asio::buffer(data), false, ec);
        REQUIRE_FALSE(ec);
        auto buf = encoder->buffer();
        return { static_cast<char const*>(buf.data()), buf.size() };
    }

    // 畸形/截断压缩数据不能抛 C++ 异常，必须落到 error_code。
    void
    feed_corrupt(std::string const& wire, httplib::compress::error expected, std::string_view encoding = "gzip")
    {
        body::any_body::value_type body = std::string {};
        http::fields fields;
        fields.set(http::field::content_encoding, encoding);

        body::any_body::reader reader(fields, body);
        boost::system::error_code ec;
        reader.init(boost::none, ec);
        REQUIRE_FALSE(ec);
        reader.put(boost::asio::buffer(wire), ec);
        if (!ec)
        {
            reader.finish(ec);
        }
        REQUIRE(ec == httplib::compress::make_error_code(expected));
    }
} // namespace

TEST_CASE("Compressor: truncated gzip body maps to incomplete, not exception", "[compressor]")
{
    auto compressed = gzip_encode(std::string(4096, 'A'));
    REQUIRE(compressed.size() >= 4);
    feed_corrupt(compressed.substr(0, compressed.size() / 2), httplib::compress::error::incomplete);
}

// boost 把"非 gzip 流/无进展"统一报为 Z_BUF_ERROR，与截断同码，无法区分。
TEST_CASE("Compressor: corrupt gzip body maps to error_code, not exception", "[compressor]")
{
    feed_corrupt(std::string("this is definitely not a gzip stream at all", 48), httplib::compress::error::incomplete);
}

TEST_CASE("Compressor: gzip checksum mismatch maps to bad_checksum", "[compressor]")
{
    auto compressed = gzip_encode(std::string(4096, 'A'));
    REQUIRE(compressed.size() >= 8);
    std::string corrupt = compressed;
    corrupt[corrupt.size() - 6] ^= 0xFF;
    feed_corrupt(corrupt, httplib::compress::error::bad_checksum);
}

TEST_CASE("Compressor: zstd bad magic maps to bad_header", "[compressor]")
{
    // zstd 帧魔数错误对应 ZSTD_error_prefix_unknown（-10）→ bad_header。
    std::string wire(32, 0);
    wire[0] ^= 0xFF;
    feed_corrupt(wire, httplib::compress::error::bad_header, "zstd");
}

TEST_CASE("Compressor: brotli roundtrip via any_body reader", "[compressor]")
{
    std::string original = "brotli roundtrip payload 0123456789";
    auto wire = encode("br", original);
    REQUIRE_FALSE(wire.empty());

    body::any_body::value_type body = std::string {};
    http::fields fields;
    fields.set(http::field::content_encoding, "br");

    body::any_body::reader reader(fields, body);
    boost::system::error_code ec;
    reader.init(boost::none, ec);
    REQUIRE_FALSE(ec);
    reader.put(boost::asio::buffer(wire), ec);
    if (!ec)
    {
        reader.finish(ec);
    }
    REQUIRE_FALSE(ec);
    REQUIRE(std::get<std::string>(body) == original);
}

TEST_CASE("Compressor: brotli corrupt input maps to error_code, not exception", "[compressor]")
{
    // 非 brotli 数据：开头即无效 → bad_header。
    feed_corrupt(std::string(64, '\0'), httplib::compress::error::bad_header, "br");
    // 合法流截断 → incomplete。
    auto wire = encode("br", std::string(4096, 'B'));
    REQUIRE_FALSE(wire.empty());
    feed_corrupt(wire.substr(0, wire.size() / 2), httplib::compress::error::incomplete, "br");
}
#endif
