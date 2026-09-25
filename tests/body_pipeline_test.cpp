#include "body/body_state.hpp"
#include "body/codec.hpp"
#include "body/sink.hpp"
#include "body/source.hpp"
#include <algorithm>
#include <boost/asio/buffer.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace body = httplib::body;
namespace net = httplib::net;
namespace json = boost::json;

namespace
{
    std::string
    pull(body::source& src)
    {
        std::string out;
        boost::system::error_code ec;
        for (;;)
        {
            auto chunk = src.next(ec);
            REQUIRE_FALSE(ec);
            if (!chunk)
            {
                break;
            }
            out.append(static_cast<char const*>(chunk->first.data()), chunk->first.size());
        }
        return out;
    }

    body::body_state
    push(body::sink& s, std::string_view data, std::size_t chunk_size = 0)
    {
        boost::system::error_code ec;
        s.init(std::nullopt, ec);
        REQUIRE_FALSE(ec);
        if (chunk_size == 0)
        {
            chunk_size = data.size();
        }
        std::size_t pos = 0;
        while (pos < data.size())
        {
            auto n = (std::min)(chunk_size, data.size() - pos);
            s.put(net::buffer(data.data() + pos, n), ec);
            REQUIRE_FALSE(ec);
            pos += n;
        }
        s.finish(ec);
        REQUIRE_FALSE(ec);
        body::body_state out;
        s.commit(out);
        return out;
    }
} // namespace

TEST_CASE("payload: set and get", "[body_pipeline]")
{
    body::body_state p;
    REQUIRE_FALSE(p.has());

    p.set<std::string>("hello");
    REQUIRE(p.has());
    REQUIRE(p.type() == body::body_state::kind::string);
    REQUIRE(p.as<std::string>() == "hello");
    REQUIRE(p.take<std::string>() == "hello");

    p.set<body::empty_tag>();
    REQUIRE(p.is_empty());

    p.set<boost::json::value>(json::value { 42 });
    REQUIRE(p.as<boost::json::value>().as_int64() == 42);
}

TEST_CASE("source: string_source once", "[body_pipeline]")
{
    std::string data = "hello world";
    body::string_source src(data);
    REQUIRE(pull(src) == "hello world");
    boost::system::error_code ec;
    REQUIRE_FALSE(src.next(ec).has_value());
}

TEST_CASE("source: json_source serializes incrementally", "[body_pipeline]")
{
    json::value value { 42 };
    body::json_source src(value);
    REQUIRE(pull(src) == "42");
}

TEST_CASE("source: query_params_source encodes", "[body_pipeline]")
{
    httplib::query_params params;
    params.add("a", "1");
    params.add("b", "2");
    body::query_params_source src(params);
    REQUIRE(pull(src) == "a=1&b=2");
}

TEST_CASE("source: empty_source yields nothing", "[body_pipeline]")
{
    body::empty_source src;
    REQUIRE(pull(src).empty());
}

TEST_CASE("sink: string_sink accumulates chunked", "[body_pipeline]")
{
    body::string_sink s;
    auto p = push(s, "abcdefghij", 3);
    REQUIRE(p.as<std::string>() == "abcdefghij");
}

TEST_CASE("sink: json_sink parses chunked", "[body_pipeline]")
{
    body::json_sink s;
    auto p = push(s, R"({"a":1,"b":[2,3]})", 4);
    REQUIRE(p.as<boost::json::value>().at("a").as_int64() == 1);
    REQUIRE(p.as<boost::json::value>().at("b").as_array().size() == 2);
}

TEST_CASE("sink: query_params_sink decodes", "[body_pipeline]")
{
    body::query_params_sink s;
    auto p = push(s, "a=1&b=2", 2);
    REQUIRE(p.as<httplib::query_params>().at<std::string>("a") == "1");
    REQUIRE(p.as<httplib::query_params>().at<std::string>("b") == "2");
}

TEST_CASE("form_data_source builds, form_data_sink parses", "[body_pipeline]")
{
    std::string boundary = "----testboundary";
    std::vector<httplib::form_data::field> fields;
    {
        httplib::form_data::field f;
        f.name = "a";
        f.content = "1";
        fields.push_back(f);
    }
    {
        httplib::form_data::field f;
        f.name = "b";
        f.content = "2";
        fields.push_back(f);
    }

    httplib::form_data form;
    form.boundary = boundary;
    form.fields = std::move(fields);
    body::form_data_source src(form);
    auto wire = pull(src);

    std::string expected = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n1\r\n--" + boundary
                           + "\r\nContent-Disposition: form-data; name=\"b\"\r\n\r\n2\r\n--" + boundary + "--\r\n";
    REQUIRE(wire == expected);

    body::form_data_sink sink("multipart/form-data; boundary=" + boundary);
    auto p = push(sink, wire, 7);
    auto const& fd = p.as<httplib::form_data>();
    REQUIRE(fd.fields.size() == 2);
    REQUIRE(fd.fields[0].name == "a");
    REQUIRE(fd.fields[0].content == "1");
    REQUIRE(fd.fields[1].name == "b");
    REQUIRE(fd.fields[1].content == "2");
}

TEST_CASE("file_source streams file content", "[body_pipeline]")
{
    auto path = httplib::fs::temp_directory_path() / "httplib_body_pipeline_file_source.bin";
    std::string content = "file source content 0123456789 abcdef";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    body::file_source src(path, {}, "", "");
    REQUIRE(src.ok());
    REQUIRE(pull(src) == content);

    httplib::html::http_ranges ranges;
    ranges.add({ 0, 4 });
    body::file_source ranged(path, ranges, "application/octet-stream", "");
    REQUIRE(ranged.ok());
    REQUIRE(pull(ranged) == content.substr(0, 5));

    std::error_code ec;
    httplib::fs::remove(path, ec);
}

#ifdef HTTPLIB_ENABLED_COMPRESS
TEST_CASE("codec: one-shot gzip roundtrip", "[body_pipeline]")
{
    std::string original(4096, 'A');
    original += "hello world";
    auto encoded = body::encode(original, "gzip");
    REQUIRE(encoded.has_value());
    REQUIRE(*encoded != original);

    auto decoded = body::decode(*encoded, "gzip");
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == original);
}

TEST_CASE("codec: stream_decoder handles small chunks", "[body_pipeline]")
{
    std::string original = "streaming decode across many small chunks 0123456789";
    auto encoded = body::encode(original, "gzip");
    REQUIRE(encoded.has_value());

    body::stream_decoder decoder;
    boost::system::error_code ec;
    decoder.reset("gzip", std::nullopt, 0, ec);
    REQUIRE_FALSE(ec);
    REQUIRE(decoder.transforms());

    std::string out;
    char buf[5];
    for (std::size_t pos = 0; pos < encoded->size(); pos += 3)
    {
        auto n = (std::min)(std::size_t(3), encoded->size() - pos);
        decoder.feed(net::buffer(encoded->data() + pos, n), ec);
        REQUIRE_FALSE(ec);
        for (;;)
        {
            auto got = decoder.drain(net::buffer(buf), ec);
            REQUIRE_FALSE(ec);
            if (got == 0)
            {
                break;
            }
            out.append(buf, got);
        }
    }
    decoder.flush(ec);
    REQUIRE_FALSE(ec);
    for (;;)
    {
        auto got = decoder.drain(net::buffer(buf), ec);
        REQUIRE_FALSE(ec);
        if (got == 0)
        {
            break;
        }
        out.append(buf, got);
    }
    REQUIRE(out == original);
}

TEST_CASE("codec: encoded_source chains compression", "[body_pipeline]")
{
    std::string original = "encoded source chain payload 0123456789 abcdefghij";
    body::encoded_source src(std::make_unique<body::string_source>(original), "gzip");
    auto wire = pull(src);

    body::stream_decoder decoder;
    boost::system::error_code ec;
    decoder.reset("gzip", std::nullopt, 0, ec);
    REQUIRE_FALSE(ec);
    decoder.feed(net::buffer(wire), ec);
    REQUIRE_FALSE(ec);
    decoder.flush(ec);
    REQUIRE_FALSE(ec);

    body::string_sink sink;
    char buf[7];
    for (;;)
    {
        auto got = decoder.drain(net::buffer(buf), ec);
        REQUIRE_FALSE(ec);
        if (got == 0)
        {
            break;
        }
        sink.put(net::buffer(buf, got), ec);
        REQUIRE_FALSE(ec);
    }
    sink.finish(ec);
    REQUIRE_FALSE(ec);
    body::body_state p;
    sink.commit(p);
    REQUIRE(p.as<std::string>() == original);
}

TEST_CASE("codec: identity passthrough", "[body_pipeline]")
{
    auto encoded = body::encode("raw bytes", "identity");
    REQUIRE(encoded.has_value());
    REQUIRE(*encoded == "raw bytes");

    body::stream_decoder decoder;
    boost::system::error_code ec;
    decoder.reset("", std::nullopt, 0, ec);
    REQUIRE_FALSE(ec);
    REQUIRE_FALSE(decoder.transforms());
    decoder.feed(net::buffer(std::string_view { "raw bytes" }), ec);
    REQUIRE_FALSE(ec);
    char buf[16];
    auto got = decoder.drain(net::buffer(buf), ec);
    REQUIRE(got == std::string_view("raw bytes").size());
    REQUIRE(std::string(buf, got) == "raw bytes");
}
#endif
