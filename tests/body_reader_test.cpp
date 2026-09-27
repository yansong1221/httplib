#include "body/body_reader.hpp"
#include "body/sink.hpp"
#include "httplib/util/async_event.hpp"
#include <array>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/_experimental/test/stream.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <catch2/catch_test_macros.hpp>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace beast = boost::beast;
namespace net = httplib::net;
namespace http = httplib::http;

namespace
{
    /// 假数据源：把预置字节经 beast::test::stream 喂给 parser。
    /// 与 body_reader 仅通过 read_some(parser, ec) 交互 —— 无需真实 socket。
    class memory_source
    {
      public:
        memory_source(net::any_io_executor ex, std::string data) : stream_(ex), data_(std::move(data))
        {
            stream_.append(data_);
            stream_.close();
        }

        template <typename Parser>
        void
        read_header(Parser& parser, boost::system::error_code& ec)
        {
            http::read_header(stream_, buffer_, parser, ec);
        }

        template <typename Parser>
        net::awaitable<void>
        read_some(Parser& parser, boost::system::error_code& ec)
        {
            co_await http::async_read_some(stream_, buffer_, parser, net::redirect_error(net::use_awaitable, ec));
        }

      private:
        beast::test::stream stream_;
        beast::flat_buffer buffer_;
        std::string data_;
    };

    std::string
    make_response(std::string const& body, std::string const& extra_headers = "")
    {
        return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra_headers + "\r\n"
               + body;
    }

    using response_reader = httplib::detail::body_reader<false, memory_source>;

    /// 在「读到 body 末尾」的那一刻把读协程挂住：此时 read_mutex_ 仍被持有，但 parser
    /// 已经 is_done()。用来复现 is_body_done() 的数据竞争 —— 锁外直读 parser 会给出
    /// "已读完"，而实际上还有数据没交给调用方。
    class hold_at_end_source
    {
      public:
        hold_at_end_source(net::any_io_executor ex,
                           std::string data,
                           std::promise<void>& reached,
                           httplib::util::async_event& release)
            : stream_(ex)
            , reached_(reached)
            , release_(release)
        {
            stream_.append(data);
            stream_.close();
        }

        template <typename Parser>
        void
        read_header(Parser& parser, boost::system::error_code& ec)
        {
            http::read_header(stream_, buffer_, parser, ec);
        }

        template <typename Parser>
        net::awaitable<void>
        read_some(Parser& parser, boost::system::error_code& ec)
        {
            co_await http::async_read_some(stream_, buffer_, parser, net::redirect_error(net::use_awaitable, ec));
            if (hold_ && !ec && parser.is_done())
            {
                hold_ = false;
                reached_.set_value();
                co_await release_.wait();
            }
        }

      private:
        beast::test::stream stream_;
        beast::flat_buffer buffer_;
        std::promise<void>& reached_;
        httplib::util::async_event& release_;
        bool hold_ = true;
    };

    response_reader
    make_reader(std::unique_ptr<memory_source> const& source,
                std::unique_ptr<http::response_parser<http::empty_body>> header_parser,
                std::uint64_t body_limit,
                net::any_io_executor ex)
    {
        return response_reader(ex, source.get(), std::move(*header_parser), body_limit);
    }
} // namespace

TEST_CASE("body_reader: materialize string body via fake source", "[body-reader]")
{
    net::io_context ioc;
    auto ex = ioc.get_executor();

    auto source = std::make_unique<memory_source>(ex, make_response("hello world"));
    auto hp = std::make_unique<http::response_parser<http::empty_body>>();
    boost::system::error_code hec;
    source->read_header(*hp, hec);
    REQUIRE_FALSE(hec);

    auto reader = make_reader(source, std::move(hp), 64 * 1024, ex);

    boost::system::error_code read_ec;
    auto fut = net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        { read_ec = co_await reader.read_body(std::make_unique<httplib::body::string_sink>()); },
        net::use_future);
    ioc.run();
    fut.get();

    REQUIRE_FALSE(read_ec);
    REQUIRE(reader.state().type() == httplib::body::body_state::kind::string);
    REQUIRE(reader.state().as<std::string>() == "hello world");
    REQUIRE(reader.is_body_done());
}

TEST_CASE("body_reader: stream raw body via fake source", "[body-reader]")
{
    net::io_context ioc;
    auto ex = ioc.get_executor();

    std::string body(10000, 'x');
    auto source = std::make_unique<memory_source>(ex, make_response(body));
    auto hp = std::make_unique<http::response_parser<http::empty_body>>();
    boost::system::error_code hec;
    source->read_header(*hp, hec);
    REQUIRE_FALSE(hec);

    auto reader = make_reader(source, std::move(hp), 1 << 20, ex);

    std::string got;
    boost::system::error_code read_ec;
    auto fut = net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            std::array<char, 512> buf {};
            for (;;)
            {
                auto n = co_await reader.read_some_raw(net::buffer(buf), read_ec);
                if (read_ec || n == 0)
                {
                    break;
                }
                got.append(buf.data(), n);
            }
        },
        net::use_future);
    ioc.run();
    fut.get();

    REQUIRE_FALSE(read_ec);
    REQUIRE(got == body);
    REQUIRE(reader.is_body_done());
}

TEST_CASE("body_reader: stream decompressed body via fake source", "[body-reader]")
{
    net::io_context ioc;
    auto ex = ioc.get_executor();

    std::string body(4096, 'y');
    auto source = std::make_unique<memory_source>(ex, make_response(body));
    auto hp = std::make_unique<http::response_parser<http::empty_body>>();
    boost::system::error_code hec;
    source->read_header(*hp, hec);
    REQUIRE_FALSE(hec);

    auto reader = make_reader(source, std::move(hp), 1 << 20, ex);

    std::string got;
    boost::system::error_code read_ec;
    auto fut = net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            std::array<char, 256> buf {};
            for (;;)
            {
                auto n = co_await reader.read_some_decompressed(net::buffer(buf), read_ec);
                if (read_ec || n == 0)
                {
                    break;
                }
                got.append(buf.data(), n);
            }
        },
        net::use_future);
    ioc.run();
    fut.get();

    REQUIRE_FALSE(read_ec);
    REQUIRE(got == body);
    REQUIRE(reader.is_body_done());
}

// 回归：body 被流式读完之后，原始字节已在调用方手里，state_ 里不会再有物化结果。
// 旧实现此时让 read_body() 返回"成功"，随后 read_json() 的 take<json::value>() 会对
// 空 state_ 抛 std::bad_variant_access —— 协程里抛异常直接逃出接口，契约被破坏。
TEST_CASE("body_reader: typed read after streaming returns error instead of throwing", "[body-reader]")
{
    net::io_context ioc;
    auto ex = ioc.get_executor();

    auto source = std::make_unique<memory_source>(ex, make_response("{\"a\":1}"));
    auto hp = std::make_unique<http::response_parser<http::empty_body>>();
    boost::system::error_code hec;
    source->read_header(*hp, hec);
    REQUIRE_FALSE(hec);

    auto reader = make_reader(source, std::move(hp), 1 << 20, ex);

    boost::system::error_code read_ec;
    bool has_value = false;
    bool has_error = false;
    auto fut = net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            // 先把 body 全部流式读走。
            std::array<char, 64> buf {};
            for (;;)
            {
                auto n = co_await reader.read_some_decompressed(net::buffer(buf), read_ec);
                if (read_ec || n == 0)
                {
                    break;
                }
            }
            REQUIRE_FALSE(read_ec);
            REQUIRE(reader.is_body_done());

            // 再要求物化：必须返回错误，且不得抛异常。read_body() 自身也不能再"假成功"。
            auto drain_ec = co_await reader.read_body();
            REQUIRE(drain_ec == boost::system::errc::make_error_code(
                                    boost::system::errc::bad_file_descriptor));

            auto result = co_await reader.read_json();
            has_value = result.has_value();
            has_error = result.has_error();
            if (has_error)
            {
                REQUIRE(result.error() == boost::system::errc::make_error_code(
                                              boost::system::errc::bad_file_descriptor));
            }
        },
        net::use_future);
    ioc.run();
    fut.get();

    REQUIRE(has_value == false);
    REQUIRE(has_error);
}

TEST_CASE("body_reader: typed read after mismatched materialize returns error", "[body-reader]")
{
    net::io_context ioc;
    auto ex = ioc.get_executor();

    auto source = std::make_unique<memory_source>(ex, make_response(""));
    auto hp = std::make_unique<http::response_parser<http::empty_body>>();
    boost::system::error_code hec;
    source->read_header(*hp, hec);
    REQUIRE_FALSE(hec);

    auto reader = make_reader(source, std::move(hp), 1 << 20, ex);

    boost::system::error_code read_ec;
    bool has_value = false;
    bool has_error = false;
    auto fut = net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            // 自动分发：无 body 时记为 empty。
            read_ec = co_await reader.read_body();
            REQUIRE_FALSE(read_ec);
            REQUIRE(reader.state().is_empty());

            // 再按 json 取：分支类型不匹配，必须返回错误而非抛 std::bad_variant_access。
            auto result = co_await reader.read_json();
            has_value = result.has_value();
            has_error = result.has_error();
            if (has_error)
            {
                REQUIRE(result.error() ==
                        boost::system::errc::make_error_code(boost::system::errc::operation_not_supported));
            }
        },
        net::use_future);
    ioc.run();
    fut.get();

    REQUIRE(has_value == false);
    REQUIRE(has_error);
}

// 回归：is_body_done() 锁外直读 state_ / parser / decoder，而这三个字段只在对端持
// read_mutex_ 读取时才被改写。SSE/NDJSON/downloader 的 "while (!is_body_done())" 轮询
// 跨线程调用时就是数据竞争；更糟的是读协程已把 parser 读到末尾、却还没把数据交给调用方
// 时，锁外会误报 "已读完"，调用方据此收手就丢数据。
TEST_CASE("body_reader: is_body_done() is race-free while a read is in flight", "[body-reader]")
{
    net::io_context ioc;
    auto ex = ioc.get_executor();

    std::string body(2048, 'z');
    std::promise<void> reached;
    auto reached_future = reached.get_future();
    httplib::util::async_event release(ex);

    auto source = std::make_unique<hold_at_end_source>(ex, make_response(body), reached, release);
    auto hp = std::make_unique<http::response_parser<http::empty_body>>();
    boost::system::error_code hec;
    source->read_header(*hp, hec);
    REQUIRE_FALSE(hec);

    using hold_reader = httplib::detail::body_reader<false, hold_at_end_source>;
    hold_reader reader(ex, source.get(), std::move(*hp), 1 << 20);

    std::string got;
    boost::system::error_code read_ec;
    auto fut = net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            std::array<char, 256> buf {};
            for (;;)
            {
                auto n = co_await reader.read_some_raw(net::buffer(buf), read_ec);
                if (read_ec || n == 0)
                {
                    break;
                }
                got.append(buf.data(), n);
            }
        },
        net::use_future);

    // 驱动 io_context 的线程独立于测试线程：is_body_done() 必须在读协程持锁期间被调用。
    std::thread th(
        [&]
        {
            ioc.run();
        });

    reached_future.wait();

    // 读协程正挂在 source 里，read_mutex_ 被持有，而 parser 已经读完。
    // 锁外直读会在这里误报 true。
    REQUIRE_FALSE(reader.is_body_done());

    release.notify_all();
    th.join();
    fut.get();

    REQUIRE_FALSE(read_ec);
    // 保守报 false 没有丢数据：body 仍然完整。
    REQUIRE(got == body);
    REQUIRE(reader.is_body_done());
}

#ifdef HTTPLIB_ENABLED_COMPRESS
// 回归：SSE/NDJSON reader 会先判 is_body_done() 再读。若实现让 is_body_done() 在解压器
// flush 之前就变 true，调用方会提前退出并丢掉 flush 才吐出的尾部解压数据。
TEST_CASE("body_reader: is_body_done() does not preempt decoder flush", "[body-reader]")
{
    net::io_context ioc;
    auto ex = ioc.get_executor();

    std::string body;
    for (int i = 0; i < 20000; ++i)
    {
        body += "payload-" + std::to_string(i) + "-abcdefghijklmnop\n";
    }

    auto wire = httplib::body::encode(body, "gzip");
    REQUIRE(wire.has_value());

    auto source = std::make_unique<memory_source>(ex, make_response(*wire, "Content-Encoding: gzip\r\n"));
    auto hp = std::make_unique<http::response_parser<http::empty_body>>();
    boost::system::error_code hec;
    source->read_header(*hp, hec);
    REQUIRE_FALSE(hec);

    auto reader = make_reader(source, std::move(hp), 1 << 20, ex);

    // 整段压缩数据一次读完（buf 远大于 wire），复现"最后一块原始数据读到即完成"的场景。
    std::string got;
    boost::system::error_code read_ec;
    auto fut = net::co_spawn(
        ioc,
        [&]() -> net::awaitable<void>
        {
            std::array<char, 1 << 16> buf {};
            while (!reader.is_body_done())
            {
                auto n = co_await reader.read_some_decompressed(net::buffer(buf), read_ec);
                if (read_ec || n == 0)
                {
                    break;
                }
                got.append(buf.data(), n);
            }
        },
        net::use_future);
    ioc.run();
    fut.get();

    REQUIRE_FALSE(read_ec);
    REQUIRE(got == body);
    REQUIRE(reader.is_body_done());
}
#endif
