#include "body/body_state.hpp"
#include "body/body_writer.hpp"
#include "body/codec.hpp"
#include "body/sink.hpp"
#include "body/source.hpp"
#include <algorithm>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/core/ignore_unused.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace body = httplib::body;
namespace net = httplib::net;
namespace http = httplib::http;
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

    /// 伪写入端：不产生任何网络字节，只记录 body_writer 交给序列化器的 body 片段，
    /// 并可按需让「写头」失败 —— 用于验证写头失败时 source 尚未被消费（重试前提）。
    class recording_task
    {
      public:
        template <typename Serializer>
        net::awaitable<void>
        write_header(Serializer& sr, boost::system::error_code& ec)
        {
            boost::ignore_unused(sr);
            if (fail_header_)
            {
                ec = boost::asio::error::connection_reset;
                co_return;
            }
            header_written_ = true;
            ec = {};
            co_return;
        }

        template <typename Serializer>
        net::awaitable<void>
        write(Serializer& sr, boost::system::error_code& ec)
        {
            auto const& body = sr.get().body();
            body_.append(static_cast<char const*>(body.data), body.size);
            more_.push_back(body.more);
            ec = {};
            co_return;
        }

        std::string const&
        body() const
        {
            return body_;
        }

        void
        reset_record()
        {
            body_.clear();
            more_.clear();
            header_written_ = false;
        }

        std::vector<bool> const&
        more() const
        {
            return more_;
        }

        bool header_written_ = false;
        bool fail_header_ = false;

      private:
        std::string body_;
        std::vector<bool> more_;
    };
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

TEST_CASE("body_writer: exposes its serializer for external changes", "[body_pipeline]")
{
    struct task
    {
    };

    using writer_t = httplib::detail::body_writer<false, task>;

    writer_t writer;
    writer.base().result(httplib::http::status::ok);
    writer.base().version(11);
    writer.content_length(0);

    auto& serializer = writer.serializer();
    serializer.split(true);
    serializer.limit(1024);

    REQUIRE(&writer.serializer() == &serializer);
    REQUIRE(&serializer.get() == &writer.message());
    REQUIRE(serializer.split());
    REQUIRE(serializer.limit() == 1024);
}

TEST_CASE("body_writer: header write failure leaves source unconsumed for retry", "[body_pipeline]")
{
    net::io_context ioc;
    auto ex = ioc.get_executor();

    recording_task task;
    task.fail_header_ = true;

    using writer_t = httplib::detail::body_writer<true, recording_task>;
    writer_t writer;
    writer.attach(&task, ex);
    writer.base().method(http::verb::post);
    writer.base().target("/upload");
    writer.base().version(11);
    writer.set_string("retry payload", "text/plain");

    boost::system::error_code ec;

    // 第一次发送：写头即失败（模拟复用池中的死连接）。
    auto fut = net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void> { ec = co_await writer.write_message(); },
        net::use_future);
    ioc.run();
    fut.get();

    // 关键不变量：写头失败时 source 一个字节都没被消费。
    REQUIRE(task.body().empty());
    REQUIRE_FALSE(task.header_written_);
    REQUIRE(ec == boost::asio::error::connection_reset);

    // 模拟 client 死连接重试：只重建 serializer（source 不动）后重发。
    task.fail_header_ = false;
    writer.reset_serializer();

    auto fut2 = net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void> { ec = co_await writer.write_message(); },
        net::use_future);
    // 上一轮 run() 因无工作而返回，io_context 处于 stopped 态，必须 restart 才能再跑。
    ioc.restart();
    ioc.run();
    fut2.get();

    REQUIRE_FALSE(ec);
    REQUIRE(task.header_written_);
    // 重发后 body 必须完整。
    REQUIRE(task.body() == "retry payload");
    REQUIRE(task.more().size() == 1);
    REQUIRE_FALSE(task.more().front());
    REQUIRE(writer.base()[http::field::content_length] == "13");
}

// 回归：换 body 必须丢掉上一个 body 留下的 Content-Length。
// prepare_payload() 见到已存在的 Content-Length 会直接沿用，于是"旧长度 + 新 body"会把对端
// 挂在那儿等永远不会到达的字节。契约：reset() 无条件丢弃 Content-Length，之后
// prepare_payload() 按当前 body 重算；要指定 Content-Length 请在 set_* 之后再设。
TEST_CASE("body_writer: reset drops the previous Content-Length", "[body_pipeline]")
{
    net::io_context ioc;
    auto ex = ioc.get_executor();

    recording_task task;

    using writer_t = httplib::detail::body_writer<true, recording_task>;
    writer_t writer;
    writer.attach(&task, ex);
    writer.base().method(http::verb::post);
    writer.base().target("/submit");
    writer.base().version(11);

    boost::system::error_code ec;

    auto send = [&]()
    {
        auto fut = net::co_spawn(
            ioc,
            [&]() -> net::awaitable<void> { ec = co_await writer.write_message(); },
            net::use_future);
        ioc.restart();
        ioc.run();
        fut.get();
        REQUIRE_FALSE(ec);
    };

    // 第一轮：长字符串（string_source 已知长度），Content-Length 由 prepare_payload() 算出。
    writer.set_string(std::string(40, 'a'), "text/plain");
    send();
    REQUIRE(task.body().size() == 40);
    REQUIRE(writer.base()[http::field::content_length] == "40");

    // 第二轮：换短 body。关键断言：Content-Length 必须重算，不能沿用 40。
    task.reset_record();
    writer.set_string("hi", "text/plain");
    send();
    REQUIRE(task.body() == "hi");
    REQUIRE(writer.base()[http::field::content_length] == "2");

    // 第三轮：换 body 时上一轮的 Content-Length 一律丢弃；空 body 走 empty_source，长度即 0。
    task.reset_record();
    writer.set_empty();
    send();
    REQUIRE(writer.base()[http::field::content_length] == "0");

    // 第四轮：要指定 Content-Length 而又不发 body（HEAD 回显 GET 长度），用 discard_body()
    // 让 source 为空、header 原样保留，而不是 set_empty()（后者会带出长度 0）。
    task.reset_record();
    writer.discard_body();
    writer.base().set(http::field::content_length, "4");
    send();
    REQUIRE(writer.base()[http::field::content_length] == "4");
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

// 回归：limit 约束的是解压"产出"字节，压缩后的长度不再从限额里扣除。
// 旧实现把压缩长度从 limit 里扣掉后（limit_ -= content_length）再对产出计数，等于把同一份
// 额度算两次：压缩后体积大、解压后仍在限额内的 body 会被误判为 body_limit。
TEST_CASE("codec: produced-bytes limit is not reduced by compressed length", "[body_pipeline]")
{
    // 用近似不可压的数据，保证压缩后体积仍接近原始大小；否则压缩长度很小，区分不出问题。
    std::string original(3000, '\0');
    std::uint32_t seed = 0x12345678u;
    for (auto& c : original)
    {
        seed = seed * 1103515245u + 12345u;
        c = static_cast<char>(seed >> 24);
    }

    auto wire = body::encode(original, "gzip");
    REQUIRE(wire.has_value());
    REQUIRE(wire->size() > 1024); // 压缩后仍较大，才足以触发旧实现的误判

    boost::system::error_code ec;

    // limit = 4096 > 解压后 3000；旧实现会变成 4096 - wire->size() < 3000 而误报。
    body::stream_decoder decoder;
    decoder.reset("gzip", std::optional<std::uint64_t>(wire->size()), 4096, ec);
    REQUIRE_FALSE(ec);
    decoder.feed(net::buffer(*wire), ec);
    REQUIRE_FALSE(ec);
    decoder.flush(ec);
    REQUIRE_FALSE(ec);

    std::string out(decoder.buffered(), '\0');
    auto n = decoder.drain(net::buffer(out), ec);
    REQUIRE_FALSE(ec);
    REQUIRE(n == original.size());
    out.resize(n);
    REQUIRE(out == original);

    // 产出确实超过限额时仍要报错，确认限额没被放宽成失效。
    body::stream_decoder bomb;
    bomb.reset("gzip", std::nullopt, 1024, ec);
    REQUIRE_FALSE(ec);
    bomb.feed(net::buffer(*wire), ec);
    if (!ec)
    {
        bomb.flush(ec);
    }
    REQUIRE(ec == http::error::body_limit);
}
#endif
