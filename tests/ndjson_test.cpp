#include "common.hpp"
#include "httplib/server/ndjson_writer.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "httplib/server/stream_writer.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>

namespace net = httplib::net;
using test_common::run;
using test_common::setup_logger;

// ===========================================================================
// NDJSON integration tests
// ===========================================================================

TEST_CASE("NDJSON: server sends single line", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ndjson",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto w = resp.create_ndjson_writer();
                    co_await w->begin();
                    boost::json::value v {
                        { "msg", "hello" },
                        {   "n",      42 }
                    };
                    co_await w->write(v, false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::client::request req(httplib::method::get, "/ndjson");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            auto ndjson = resp.create_ndjson_reader();

            std::vector<boost::json::value> items;
            co_await collect_ndjson_lines(*ndjson, items);

            REQUIRE(items.size() == 1);
            REQUIRE(items[0].at("msg") == "hello");
            REQUIRE(items[0].at("n") == 42);

            co_return;
        });
}

TEST_CASE("NDJSON: server sends multiple lines", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ndjson",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto w = resp.create_ndjson_writer();
                    co_await w->begin();
                    co_await w->write(
                        {
                            { "i", 1 }
                    },
                        true);
                    co_await w->write(
                        {
                            { "i", 2 }
                    },
                        true);
                    co_await w->write(
                        {
                            { "i", 3 }
                    },
                        false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::client::request req(httplib::method::get, "/ndjson");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            auto ndjson = resp.create_ndjson_reader();

            std::vector<boost::json::value> items;
            co_await collect_ndjson_lines(*ndjson, items);

            REQUIRE(items.size() == 3);
            REQUIRE(items[0].at("i") == 1);
            REQUIRE(items[1].at("i") == 2);
            REQUIRE(items[2].at("i") == 3);

            co_return;
        });
}

TEST_CASE("NDJSON: Content-Type is application/x-ndjson", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ndjson",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto w = resp.create_ndjson_writer();
                    co_await w->begin();
                    co_await w->write(
                        {
                            { "x", 1 }
                    },
                        false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::client::request req(httplib::method::get, "/ndjson");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            auto ndjson = resp.create_ndjson_reader();

            REQUIRE(resp[httplib::field::content_type] == "application/x-ndjson");

            co_return;
        });
}

TEST_CASE("NDJSON: reader stops early", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ndjson",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto w = resp.create_ndjson_writer();
                    co_await w->begin();
                    co_await w->write(
                        {
                            { "i", 1 }
                    },
                        true);
                    co_await w->write(
                        {
                            { "i", 2 }
                    },
                        true);
                    co_await w->write(
                        {
                            { "i", 3 }
                    },
                        false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::client::request req(httplib::method::get, "/ndjson");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            auto ndjson = resp.create_ndjson_reader();

            std::vector<boost::json::value> items;
            for (int i = 0; i < 2; ++i)
            {
                auto result = co_await ndjson->read();
                if (result.has_error())
                {
                    break;
                }
                auto& val = result.value();
                if (val.is_null())
                {
                    break;
                }
                items.push_back(std::move(val));
            }

            REQUIRE(items.size() == 2);

            co_return;
        });
}

// ===========================================================================
// NDJSON chunk-boundary tests (server sends split lines via stream_writer)
// ===========================================================================

TEST_CASE("NDJSON: single line split across chunks", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ndjson",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto cw = resp.create_stream_writer();
                    httplib::headers headers;
                    headers.set(httplib::field::content_type, "application/x-ndjson");
                    co_await cw->write_header(httplib::status::ok, headers, httplib::server::stream_writer::mode::chunked);
                    co_await cw->write_body(net::buffer(std::string("{\"a\":1}")), true);
                    co_await cw->write_body(net::buffer(std::string("\n")), false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::client::request req(httplib::method::get, "/ndjson");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            auto ndjson = resp.create_ndjson_reader();

            std::vector<boost::json::value> items;
            co_await collect_ndjson_lines(*ndjson, items);
            REQUIRE(items.size() == 1);
            REQUIRE(items[0].at("a") == 1);

            co_return;
        });
}

TEST_CASE("NDJSON: multiple lines in one chunk", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ndjson",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto cw = resp.create_stream_writer();
                    httplib::headers headers;
                    headers.set(httplib::field::content_type, "application/x-ndjson");
                    co_await cw->write_header(httplib::status::ok, headers, httplib::server::stream_writer::mode::chunked);
                    co_await cw->write_body(net::buffer(std::string("{\"a\":1}\n{\"b\":2}\n")), false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::client::request req(httplib::method::get, "/ndjson");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            auto ndjson = resp.create_ndjson_reader();

            std::vector<boost::json::value> items;
            co_await collect_ndjson_lines(*ndjson, items);
            REQUIRE(items.size() == 2);
            REQUIRE(items[0].at("a") == 1);
            REQUIRE(items[1].at("b") == 2);

            co_return;
        });
}

TEST_CASE("NDJSON: partial line split across chunks", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ndjson",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto cw = resp.create_stream_writer();
                    httplib::headers headers;
                    headers.set(httplib::field::content_type, "application/x-ndjson");
                    co_await cw->write_header(httplib::status::ok, headers, httplib::server::stream_writer::mode::chunked);
                    co_await cw->write_body(net::buffer(std::string("{\"x\":100}\n{\"y\":")), true);
                    co_await cw->write_body(net::buffer(std::string("200}\n")), false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::client::request req(httplib::method::get, "/ndjson");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            auto ndjson = resp.create_ndjson_reader();

            std::vector<boost::json::value> items;
            co_await collect_ndjson_lines(*ndjson, items);
            REQUIRE(items.size() == 2);
            REQUIRE(items[0].at("x") == 100);
            REQUIRE(items[1].at("y") == 200);

            co_return;
        });
}

TEST_CASE("NDJSON: lines are delivered incrementally", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ndjson",
                [](httplib::server::request&, httplib::server::response& resp) -> net::awaitable<void>
                {
                    auto w = resp.create_ndjson_writer();
                    co_await w->begin();
                    co_await w->write(
                        {
                            { "i", 1 }
                    },
                        true);

                    // 第二条延迟 1s；客户端必须在第一条到达后立即返回，而不是等它。
                    auto ex = co_await net::this_coro::executor;
                    net::steady_timer timer(ex, std::chrono::seconds(1));
                    boost::system::error_code ec;
                    co_await timer.async_wait(net::redirect_error(net::use_awaitable, ec));

                    co_await w->write(
                        {
                            { "i", 2 }
                    },
                        false);
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::client::request req(httplib::method::get, "/ndjson");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            auto ndjson = resp.create_ndjson_reader();

            auto begin = std::chrono::steady_clock::now();
            auto first = co_await ndjson->read();
            auto elapsed = std::chrono::steady_clock::now() - begin;

            REQUIRE(first.has_value());
            REQUIRE(first.value().at("i") == 1);
            REQUIRE(elapsed < std::chrono::milliseconds(500));

            co_return;
        });
}

// 回归：末行不带换行时，这条记录必须照样交出来。
// RFC 7464 §2 允许最后一行省略结尾换行，库外的 NDJSON 生产者这么写很常见；而本库的
// ndjson_writer 总是补 "\n"（ndjson_writer_impl::write），所以只有绕开 writer、直接给
// 一个定长 body 才走得到这条路径。
//
// 旧实现在"没有 \n 且 body 已读完"时直接返回空 value，把 buf_ 里的残留整段丢掉且不清
// buf_：collect_ndjson_lines 读到 null 就 break，于是调用方拿到的是一个看起来干净、实际
// 少了一条的结果流；只按 is_done() 推进的调用方更会一直读到空 value 停不下来。
TEST_CASE("NDJSON: last record without trailing newline is still delivered", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ndjson-no-trailing-nl",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    // 注意结尾没有 \n。
                    resp.set_string_content("{\"i\":1}\n{\"i\":2}\n{\"i\":3}"sv,"application/x-ndjson");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::client::request req(httplib::method::get, "/ndjson-no-trailing-nl");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            auto ndjson = resp.create_ndjson_reader();

            std::vector<boost::json::value> items;
            co_await collect_ndjson_lines(*ndjson, items);

            REQUIRE(items.size() == 3);
            REQUIRE(items[0].at("i") == 1);
            REQUIRE(items[1].at("i") == 2);
            REQUIRE(items[2].at("i") == 3);
            // 读完之后必须真的 done，否则只按 is_done() 推进的调用方会卡住。
            REQUIRE(ndjson->is_done());

            co_return;
        });
}

// 只有一条记录、且不带结尾换行：残留就是全部内容，不能被当成"没有记录"。
TEST_CASE("NDJSON: single record without trailing newline", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ndjson-single-no-nl",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("{\"i\":7}"sv, "application/x-ndjson"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::client::request req(httplib::method::get, "/ndjson-single-no-nl");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            auto ndjson = resp.create_ndjson_reader();

            std::vector<boost::json::value> items;
            co_await collect_ndjson_lines(*ndjson, items);

            REQUIRE(items.size() == 1);
            REQUIRE(items[0].at("i") == 7);
            REQUIRE(ndjson->is_done());

            co_return;
        });
}

// 末行残留是空行（只有 \r）时不算记录，仍返回空 value 作结束信号。
TEST_CASE("NDJSON: trailing CR without newline is not a record", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ndjson-trailing-cr",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("{\"i\":1}\n\r"sv, "application/x-ndjson"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::client::request req(httplib::method::get, "/ndjson-trailing-cr");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            auto ndjson = resp.create_ndjson_reader();

            std::vector<boost::json::value> items;
            co_await collect_ndjson_lines(*ndjson, items);

            REQUIRE(items.size() == 1);
            REQUIRE(items[0].at("i") == 1);
            REQUIRE(ndjson->is_done());

            co_return;
        });
}

// 末行被截断（不是完整 JSON）时必须以 error_code 报回来，不能抛异常：read() 的返回类型
// 就是 boost::system::result<value>，调用方按契约判 has_error()。旧实现用 boost::json::parse
// 的抛异常重载，于是异常会抛穿协程，has_error() 成了死代码。
// 另一面同样重要：末行残留必须真的被送去解析（旧实现直接丢弃并返回空 value），否则数据
// 损坏会被伪装成一次干净的流结束。
TEST_CASE("NDJSON: truncated final record reports a parse error", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ndjson-truncated",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("{\"i\":1}\n{\"i\":"sv, "application/x-ndjson"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::client::request req(httplib::method::get, "/ndjson-truncated");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            auto ndjson = resp.create_ndjson_reader();

            auto first = co_await ndjson->read();
            REQUIRE(first.has_value());
            REQUIRE(first->at("i") == 1);

            auto second = co_await ndjson->read();
            REQUIRE(second.has_error());

            co_return;
        });
}

// 流中间夹一个坏行：同样必须返回 error 而不是抛。这是旧实现另一处抛异常重载的所在
// （末行那条走 finish_tail()，这条走带 \n 的常规路径），两条都得守住。
TEST_CASE("NDJSON: malformed mid-stream line reports a parse error", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ndjson-bad-mid",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("{\"i\":1}\nnot json\n{\"i\":3}\n"sv, "application/x-ndjson"); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::client::request req(httplib::method::get, "/ndjson-bad-mid");
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            auto ndjson = resp.create_ndjson_reader();

            auto first = co_await ndjson->read();
            REQUIRE(first.has_value());
            REQUIRE(first->at("i") == 1);

            // 关键：不能抛。collect_ndjson_lines 就是靠这个 has_error() 停下来的。
            auto second = co_await ndjson->read();
            REQUIRE(second.has_error());

            co_return;
        });
}

#ifdef HTTPLIB_ENABLED_COMPRESS
TEST_CASE("NDJSON: reader decodes gzip-compressed stream", "[ndjson]")
{
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ndjson-gzip",
                [](httplib::server::request&, httplib::server::response& resp)
                { resp.set_string_content("{\"msg\":\"hello\",\"n\":42}\n"sv, "application/json"sv); });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::headers headers;
            headers.set(httplib::field::accept_encoding, "gzip");
            httplib::client::request req(httplib::method::get, "/ndjson-gzip", headers);
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp[httplib::field::content_encoding] == "gzip");

            auto ndjson = resp.create_ndjson_reader();
            std::vector<boost::json::value> items;
            co_await collect_ndjson_lines(*ndjson, items);
            REQUIRE(items.size() == 1);
            REQUIRE(items[0].at("msg") == "hello");
            REQUIRE(items[0].at("n") == 42);
            co_return;
        });
}

// 回归：大体积 gzip 流必须完整解出。此前 read_some_decompressed_locked 在最后一块
// 原始数据读到且 parser 恰好完成时不会 flush 解压器，而 is_body_done() 会先返回 true，
// 导致 NDJSON reader 提前结束、丢掉 flush 才吐出的尾部数据。
TEST_CASE("NDJSON: reader decodes large gzip stream without truncation", "[ndjson]")
{
    constexpr int kCount = 5000;
    run(
        [](auto& server)
        {
            server.router().template set_http_handler<httplib::method::get>(
                "/ndjson-gzip-large",
                [](httplib::server::request&, httplib::server::response& resp)
                {
                    std::string body;
                    body.reserve(std::size_t(kCount) * 12);
                    for (int i = 0; i < kCount; ++i)
                    {
                        body += "{\"i\":" + std::to_string(i) + "}\n";
                    }
                    resp.set_string_content(body, "application/json");
                });
        },
        [](auto& client) -> net::awaitable<void>
        {
            httplib::headers headers;
            headers.set(httplib::field::accept_encoding, "gzip");
            httplib::client::request req(httplib::method::get, "/ndjson-gzip-large", headers);
            auto resp = UNWRAP(co_await client.async_send_request(req, httplib::client::http_client::body_mode::lazy));
            REQUIRE(resp.result() == httplib::status::ok);
            REQUIRE(resp[httplib::field::content_encoding] == "gzip");

            auto ndjson = resp.create_ndjson_reader();
            std::vector<boost::json::value> items;
            co_await collect_ndjson_lines(*ndjson, items);
            REQUIRE(items.size() == kCount);
            for (int i = 0; i < kCount; ++i)
            {
                REQUIRE(items[i].at("i") == i);
            }
            co_return;
        });
}
#endif
