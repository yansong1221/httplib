#include "body/body_reader.hpp"
#include "body/sink.hpp"
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
#include <memory>
#include <string>
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
            co_await http::async_read_some(stream_,
                                           buffer_,
                                           parser,
                                           net::redirect_error(net::use_awaitable, ec));
        }

      private:
        beast::test::stream stream_;
        beast::flat_buffer buffer_;
        std::string data_;
    };

    std::string
    make_response(std::string const& body, std::string const& extra_headers = "")
    {
        return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n"
               + extra_headers + "\r\n" + body;
    }

    using response_reader = httplib::detail::body_reader<false, memory_source>;

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
        {
            read_ec = co_await reader.read_body(std::make_unique<httplib::body::string_sink>());
        },
        net::use_future);
    ioc.run();
    fut.get();

    REQUIRE_FALSE(read_ec);
    REQUIRE(reader.state().type() == httplib::body::body_state::kind::string);
    REQUIRE(reader.state().as_string() == "hello world");
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
