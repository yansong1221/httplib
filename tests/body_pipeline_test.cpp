#include "beast_alias.hpp"
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
namespace http = httplib::http;
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

    /// 浼啓鍏ョ锛氫笉浜х敓浠讳綍缃戠粶瀛楄妭锛屽彧璁板綍 body_writer 浜ょ粰搴忓垪鍖栧櫒鐨?body 鐗囨锛?
    /// 骞跺彲鎸夐渶璁┿€屽啓澶淬€嶅け璐?鈥斺€?鐢ㄤ簬楠岃瘉鍐欏ご澶辫触鏃?source 灏氭湭琚秷璐癸紙閲嶈瘯鍓嶆彁锛夈€?
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
    writer.base().result(http::status::ok);
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

    // 绗竴娆″彂閫侊細鍐欏ご鍗冲け璐ワ紙妯℃嫙澶嶇敤姹犱腑鐨勬杩炴帴锛夈€?
    auto fut = net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void> { ec = co_await writer.write_message(); },
        net::use_future);
    ioc.run();
    fut.get();

    // 鍏抽敭涓嶅彉閲忥細鍐欏ご澶辫触鏃?source 涓€涓瓧鑺傞兘娌¤娑堣垂銆?
    REQUIRE(task.body().empty());
    REQUIRE_FALSE(task.header_written_);
    REQUIRE(ec == boost::asio::error::connection_reset);

    // 妯℃嫙 client 姝昏繛鎺ラ噸璇曪細鍙噸寤?serializer锛坰ource 涓嶅姩锛夊悗閲嶅彂銆?
    task.fail_header_ = false;
    writer.reset_serializer();

    auto fut2 = net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void> { ec = co_await writer.write_message(); },
        net::use_future);
    // 涓婁竴杞?run() 鍥犳棤宸ヤ綔鑰岃繑鍥烇紝io_context 澶勪簬 stopped 鎬侊紝蹇呴』 restart 鎵嶈兘鍐嶈窇銆?
    ioc.restart();
    ioc.run();
    fut2.get();

    REQUIRE_FALSE(ec);
    REQUIRE(task.header_written_);
    // 閲嶅彂鍚?body 蹇呴』瀹屾暣銆?
    REQUIRE(task.body() == "retry payload");
    REQUIRE(task.more().size() == 1);
    REQUIRE_FALSE(task.more().front());
    REQUIRE(writer.base()[http::field::content_length] == "13");
}

// 鍥炲綊锛氭崲 body 蹇呴』涓㈡帀涓婁竴涓?body 鐣欎笅鐨?Content-Length銆?
// prepare_payload() 瑙佸埌宸插瓨鍦ㄧ殑 Content-Length 浼氱洿鎺ユ部鐢紝浜庢槸"鏃ч暱搴?+ 鏂?body"浼氭妸瀵圭
// 鎸傚湪閭ｅ効绛夋案杩滀笉浼氬埌杈剧殑瀛楄妭銆傚绾︼細reset() 鏃犳潯浠朵涪寮?Content-Length锛屼箣鍚?
// prepare_payload() 鎸夊綋鍓?body 閲嶇畻锛涜鎸囧畾 Content-Length 璇峰湪 set_* 涔嬪悗鍐嶈銆?
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

    // 绗竴杞細闀垮瓧绗︿覆锛坰tring_source 宸茬煡闀垮害锛夛紝Content-Length 鐢?prepare_payload() 绠楀嚭銆?
    writer.set_string(std::string(40, 'a'), "text/plain");
    send();
    REQUIRE(task.body().size() == 40);
    REQUIRE(writer.base()[http::field::content_length] == "40");

    // 绗簩杞細鎹㈢煭 body銆傚叧閿柇瑷€锛欳ontent-Length 蹇呴』閲嶇畻锛屼笉鑳芥部鐢?40銆?
    task.reset_record();
    writer.set_string("hi", "text/plain");
    send();
    REQUIRE(task.body() == "hi");
    REQUIRE(writer.base()[http::field::content_length] == "2");

    // 绗笁杞細鎹?body 鏃朵笂涓€杞殑 Content-Length 涓€寰嬩涪寮冿紱绌?body 璧?empty_source锛岄暱搴﹀嵆 0銆?
    task.reset_record();
    writer.set_empty();
    send();
    REQUIRE(writer.base()[http::field::content_length] == "0");

    // 绗洓杞細瑕佹寚瀹?Content-Length 鑰屽張涓嶅彂 body锛圚EAD 鍥炴樉 GET 闀垮害锛夛紝鐢?discard_body()
    // 璁?source 涓虹┖銆乭eader 鍘熸牱淇濈暀锛岃€屼笉鏄?set_empty()锛堝悗鑰呬細甯﹀嚭闀垮害 0锛夈€?
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
    body::form_data_source src(std::move(form));
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

    std::ifstream file(path, std::ios::binary | std::ios::in);
    REQUIRE(file.is_open());
    body::file_source src(std::move(file), {}, "", "");
    REQUIRE(pull(src) == content);

    httplib::html::http_ranges ranges;
    ranges.add({ 0, 4 });
    std::ifstream ranged_file(path, std::ios::binary | std::ios::in);
    REQUIRE(ranged_file.is_open());
    body::file_source ranged(std::move(ranged_file), ranges, "application/octet-stream", "");
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
    decoder.reset("gzip", 0, ec);
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
    decoder.reset("", 0, ec);
    REQUIRE_FALSE(ec);
    REQUIRE_FALSE(decoder.transforms());
    decoder.feed(net::buffer(std::string_view { "raw bytes" }), ec);
    REQUIRE_FALSE(ec);
    char buf[16];
    auto got = decoder.drain(net::buffer(buf), ec);
    REQUIRE(got == std::string_view("raw bytes").size());
    REQUIRE(std::string(buf, got) == "raw bytes");
}

// 鍥炲綊锛歭imit 绾︽潫鐨勬槸瑙ｅ帇"浜у嚭"瀛楄妭锛屽帇缂╁悗鐨勯暱搴︿笉鍐嶄粠闄愰閲屾墸闄ゃ€?
// 鏃у疄鐜版妸鍘嬬缉闀垮害浠?limit 閲屾墸鎺夊悗锛坙imit_ -= content_length锛夊啀瀵逛骇鍑鸿鏁帮紝绛変簬鎶婂悓涓€浠?
// 棰濆害绠椾袱娆★細鍘嬬缉鍚庝綋绉ぇ銆佽В鍘嬪悗浠嶅湪闄愰鍐呯殑 body 浼氳璇垽涓?body_limit銆?
TEST_CASE("codec: produced-bytes limit is not reduced by compressed length", "[body_pipeline]")
{
    // 鐢ㄨ繎浼间笉鍙帇鐨勬暟鎹紝淇濊瘉鍘嬬缉鍚庝綋绉粛鎺ヨ繎鍘熷澶у皬锛涘惁鍒欏帇缂╅暱搴﹀緢灏忥紝鍖哄垎涓嶅嚭闂銆?
    std::string original(3000, '\0');
    std::uint32_t seed = 0x12345678u;
    for (auto& c : original)
    {
        seed = seed * 1103515245u + 12345u;
        c = static_cast<char>(seed >> 24);
    }

    auto wire = body::encode(original, "gzip");
    REQUIRE(wire.has_value());
    REQUIRE(wire->size() > 1024); // 鍘嬬缉鍚庝粛杈冨ぇ锛屾墠瓒充互瑙﹀彂鏃у疄鐜扮殑璇垽

    boost::system::error_code ec;

    // limit = 4096 > 瑙ｅ帇鍚?3000锛涙棫瀹炵幇浼氬彉鎴?4096 - wire->size() < 3000 鑰岃鎶ャ€?
    body::stream_decoder decoder;
    decoder.reset("gzip", 4096, ec);
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

    // 浜у嚭纭疄瓒呰繃闄愰鏃朵粛瑕佹姤閿欙紝纭闄愰娌¤鏀惧鎴愬け鏁堛€?
    body::stream_decoder bomb;
    bomb.reset("gzip", 1024, ec);
    REQUIRE_FALSE(ec);
    bomb.feed(net::buffer(*wire), ec);
    if (!ec)
    {
        bomb.flush(ec);
    }
    REQUIRE(ec == http::error::body_limit);
}

// 回归：拿 Content-Length（线上压缩长度）复查一遍额度，两个毛病同时暴露。
//
//  1. 量纲错：压缩长度和 limit 比的是"线上字节"，而 limit 约束的是"解压产出"。取不可压数据
//     让 gzip 反而比原文更长，于是"压缩长度 ≥ limit、解压产出 ≤ limit"完全合法，却会被拒。
//  2. 差一格：快速路径用 >=，而 account() 用 >、Beast 的 body_limit 也是 >（恰好 limit 放行）。
//     所以哪怕量纲对，恰好等于 limit 的 body 也会被这道多余的检查拒掉。
//
// 上面的用例测不到这两点：它 limit=4096 而压缩体约 1030~3010，快速路径根本不触发。
// 旧实现下本用例在 reset() 那一刻就报 body_limit（压根没进 feed）。
TEST_CASE("codec: compressed Content-Length must not pre-reject a within-limit body", "[body_pipeline]")
{
    // 伪随机（近似不可压）数据，gzip 之后只会略微变大。
    std::string original(1024, '\0');
    std::uint32_t seed = 0xC0FFEEu;
    for (auto& c : original)
    {
        seed = seed * 1103515245u + 12345u;
        c = static_cast<char>(seed >> 24);
    }

    auto wire = body::encode(original, "gzip");
    REQUIRE(wire.has_value());
    // 前提：压缩后比 limit 还大（>= 触发旧快速路径），解压产出恰好等于 limit。
    REQUIRE(wire->size() >= 1024);

    boost::system::error_code ec;
    body::stream_decoder decoder;
    decoder.reset("gzip", 1024, ec);
    REQUIRE_FALSE(ec);
    decoder.feed(net::buffer(*wire), ec);
    REQUIRE_FALSE(ec);
    decoder.flush(ec);
    REQUIRE_FALSE(ec);

    std::string out(decoder.buffered(), '\0');
    auto n = decoder.drain(net::buffer(out), ec);
    REQUIRE_FALSE(ec);
    out.resize(n);
    // 产出恰好 1024 == limit：边界是 >，应当放行（与 Beast 一致）。
    REQUIRE(n == 1024);
    REQUIRE(out == original);

    // 边界另一侧仍要拒：limit 比产出小 1 就得报。
    body::stream_decoder tight;
    tight.reset("gzip", 1023, ec);
    REQUIRE_FALSE(ec);
    tight.feed(net::buffer(*wire), ec);
    if (!ec)
    {
        tight.flush(ec);
    }
    REQUIRE(ec == http::error::body_limit);
}
#endif

// 回归：空 body 声明了 Content-Encoding 时不能变成硬错误。
// flush() 曾无条件对解压器调 finish()，而空 body 一个字节都没喂过——gzip 解压器没见到
// 头，finish() 报 "compression stream truncated or insufficient input"（httplib.compress/6），
// 于是 read_string()/read() 返回错误而不是空串。本库 server 协商压缩时不检查 body 是否为空
// （session.cpp），HEAD 响应也会带上 GET 的 Content-Encoding，所以这是可达路径。
TEST_CASE("stream_decoder: flushing a never-fed decoder is an empty success", "[body]")
{
    boost::system::error_code ec;

    SECTION("gzip, nothing ever fed")
    {
        body::stream_decoder d;
        d.reset("gzip", 1024, ec);
        REQUIRE_FALSE(ec);
        REQUIRE(d.transforms());
        d.flush(ec);
        REQUIRE_FALSE(ec);
        REQUIRE(d.buffered() == 0);
    }

    SECTION("gzip, fed zero bytes")
    {
        body::stream_decoder d;
        d.reset("gzip", 1024, ec);
        REQUIRE_FALSE(ec);
        d.feed(net::buffer("", 0), ec);
        REQUIRE_FALSE(ec);
        d.flush(ec);
        REQUIRE_FALSE(ec);
    }

    SECTION("one-shot decode of an empty gzip body yields an empty string")
    {
        auto r = body::decode("", "gzip", 1024);
        REQUIRE(r);
        REQUIRE(r->empty());
    }

    SECTION("a real gzip stream still decodes and still validates its trailer")
    {
        // 非空输入必须仍然走 finish()，否则截断的 gzip 流会被当成成功。
        auto wire = body::encode("hello", "gzip");
        REQUIRE(wire);
        body::stream_decoder d;
        d.reset("gzip", 1024, ec);
        REQUIRE_FALSE(ec);
        d.feed(net::buffer(*wire), ec);
        REQUIRE_FALSE(ec);
        d.flush(ec);
        REQUIRE_FALSE(ec);
        std::string out;
        out.resize(d.buffered());
        d.drain(net::buffer(out), ec);
        REQUIRE_FALSE(ec);
        REQUIRE(out == "hello");
    }
}

TEST_CASE("stream_encoder: encoding an empty string still emits a valid gzip stream", "[body]")
{
    // 与 decoder 的空输入跳过相反：编码空串必须产出合法空载荷流，否则对端拿到 0 字节。
    auto wire = body::encode("", "gzip");
    REQUIRE(wire);
    REQUIRE(wire->size() > 0);
    REQUIRE(body::decode(*wire, "gzip", 1024));
}