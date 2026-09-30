#pragma once
#include "feature_flags.hpp"
#include "httplib/client/client.hpp"
#include "httplib/client/stream_reader.hpp"
#include "httplib/server/router.hpp"
#include "httplib/server/server.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <chrono>
#include <memory>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>
#include <vector>

#ifdef _DEBUG
#include <crtdbg.h>
namespace
{
    // Debug CRT 断言（如 Catch2 打印含高位字节的断言展开时）默认弹模态框，
    // 会阻塞单线程 io_context 导致测试套件假死。改为打印到 stderr 正常失败。
    struct suppress_crt_assert_dialog
    {
        suppress_crt_assert_dialog()
        {
            _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG | _CRTDBG_MODE_FILE);
            _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        }
    };
    suppress_crt_assert_dialog g_suppress;
} // namespace
#endif

using namespace std::string_view_literals;
namespace net = httplib::net;

#ifdef HTTPLIB_ENABLED_SSL
constexpr auto kTestCert = R"(-----BEGIN CERTIFICATE-----
MIIDKDCCAhACCQDHu0UVVUEr4DANBgkqhkiG9w0BAQsFADBWMQswCQYDVQQGEwJD
TjEVMBMGA1UEBwwMRGVmYXVsdCBDaXR5MRwwGgYDVQQKDBNEZWZhdWx0IENvbXBh
bnkgTHRkMRIwEAYDVQQDDAlsb2NhbGhvc3QwHhcNMjIxMDI1MDM1NzMwWhcNMzIx
MDIyMDM1NzMwWjBWMQswCQYDVQQGEwJDTjEVMBMGA1UEBwwMRGVmYXVsdCBDaXR5
MRwwGgYDVQQKDBNEZWZhdWx0IENvbXBhbnkgTHRkMRIwEAYDVQQDDAlsb2NhbGhv
c3QwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQCr6iWgRRYJ9QfKSUPT
nbw2rKZRlSBqnLeLdPam+s8RUA1p+YPoH2HJqIdxcfYmToz5t6G5OX8TFhAssShw
PalRlQm5QHp4pL7nqPV79auB3PYKv6TgOumwDUpoBxcu0l9di9fjYbC2LmpVJeVz
WQxCo+XO/g5YjXN1nPPeBgmZVkRvXLIYCTKshLlUa0nW7hj7Sl8CAV8OBNMBFkf1
2vgcTqhs3yW9gnIwIoCFZvsdAsSbwR6zF1z96MeAYDIZWeyzUXkoZa4OCWwAhqzo
+0JWukuNuHhsQhIJDvIZWHEblT0GlentP8HPXjFnJHYGUAjx3Fj1mH8mFG0fEXXN
06qlAgMBAAEwDQYJKoZIhvcNAQELBQADggEBAGbKTy1mfSlJF012jKuIue2valI2
CKz8X619jmxxIzk0k7wcmAlUUrUSFIzdIddZj92wYbBC1YNOWQ4AG5zpFo3NAQaZ
kYGnlt+d2pNHLaT4IV9JM4iwTqPyi+FsOwTjUGHgaOr+tfK8fZmPbDmAE46OlC/a
VVqNPmjaJiM2c/pJOs+HV9PvEOFmV9p5Yjjz4eV3jwqHdOcxZuLJl28/oqz65uCu
LQiivkdVCuwc1IlpRFejkrbkrk28XCCJwokLt03EQj4xs0sjoTKgd92fpjls/tt+
rw+7ILsAsuoWPIdiuCArCU1LXJDz3FDHafX/dxzdVBzpfVgP0rNpS050Mls=
-----END CERTIFICATE-----
)"sv;

constexpr auto kTestKey = R"(-----BEGIN RSA PRIVATE KEY-----
Proc-Type: 4,ENCRYPTED
DEK-Info: DES-EDE3-CBC,D920B8941C56ADDC

I2lW3QsAG/xubjtXpXh3wQ5Ru3VZiMkPNjc+G6/2JjjVr1sD+fzCWvvwdqdxGuNJ
gKdpPBHLuQfTTzGETE4NKDkYzmiPTVbZPJ77DyfL2cK1dcZtAY46RsHf+VMI5N8l
Be1jQSB5xvUa88dSIeowPTc2XSnTIoSFWCa38XuqYF7i0a3lv96eAyXpqB7Tm2r8
SoYlm0n7/uzRpk6HWST65qnVv/j+37LuvSy6ehyh44+KDS4x9FUOZc5xwJ/37Jnl
SDC10+9zLc+jOTk6XgUuBSmG+xfZdcOrbknQ1Xj1YtseYH0plYAEWi4PsnMQkHzC
GGvK08Lgqxd7cGEKFh2MRZ/TEwriN5ud5HGm4yIHIj45rbedtRSQwl2EyHdWeW0J
rFltDy+SXnnkJaOcnBYXUD1jEwyy2lLamWRiu83VFbCv6yhOYuR6JejM6dctjgZ+
Qf0PzH6L1bVpHKEl/GLByJ6GWYrQJqw83LAXlR+NNCC3nN7WAAaTuzA9LpgW9Vk0
khRRs7rJGxwwwE4TfG9FbQxwuOsjKV9pRohB1x1nFMMm5IJ9SON2KjizsVdLbt7t
Gb/5M7RcSnnGvIWWXalXpFGKgciwYd8F1v0TJ+FMooZxgUp7Pmp5YKIHkBjMrnnW
rKuoxmA5oPgSNUtr4ddMJ1sTIQPhqI27+CrySTzWKH1ls45okBvsiCejpcJwfrZW
KLSkz/FsPoWm44uomBSDOikry8axrKQLB9tOVPKCx/z0VP060P9N81mu4h67bixr
xu+odIONqGhRZT/BYHL2NjDfWlFmTJQy8Drn1a7IEhp8FV7l2aY/hisrMN7MQVza
FGB0hMbVHGeFOCD9QNQwRU2wLtwpE7LT/lGNmKadQadXxeAqOWBckXrpwnrxZDEP
a8AYr2J55h/IE4Oi2DyibSEZdB+7334OJHMmr14q53eIpeit19BYVhWyu9AtORJp
As61C7s82AO+E5gOswsq05jwWV/GIIkgZ8/vswEffiihmDEf6AUZsVGW3BlpFlyU
i3g4e8HFTJ+s9Z3sTgZ1EWOP6Wd2OzyQYVA4ggBR/g/IC9s5em1wvAkVwIZaPvj7
21BIQXyiGrw52T+vTUrAUG0l7yoHGCgVYJ+aEm+f103AiBYuReUbo39GEIY2GHLu
r3oUehtt4of0ootmPCmjrRUyY6LPeD+d+i1jJUSYFKezsVRpaiF5+J8YLGMcOPiI
8qRRNgXDMMvttwyhoxyr5+667OMv+XWr2VQj7i9MWCFwTMwNzdUoZI3PWDhXbXDO
lQJS6v3iAPw+KvLJywODe+C4shUqYdrRdUSKE0FfuB8Ajzh86+FmjJcZM+BSxM4J
hC2yjv114jDlsgjFSxQE2K1iotLUY9mfmW8QWVMO3L4LlNpr4ypNLYX0Ph2wgqzQ
kszXTFN11RFKFLUhF0Mi5m4ffMLPD5YyoqO9grpyC1Nt7vxaPPvcvPD86jK3ksqJ
MwucZGgm9HtUuAjGOSljUr0d+d+4pySJbcpH2YDIBHGVsCScYPVg8XZ1CYko3mq/
d6jDUgydraEmQvIPiKMpTE18rW+jierv2FlB8AGcwxm2VWxuM25wQ40J2YuZLY7k
-----END RSA PRIVATE KEY-----
)"sv;
#endif

namespace test_common
{

    inline void
    setup_logger(httplib::server::http_server& server)
    {
        auto null_sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        server.set_logger(std::make_shared<spdlog::logger>("test", null_sink));
    }

    /// 裸 socket 发一条原始 HTTP 请求，读到对端关闭为止，原样返回响应字节。
    ///
    /// httplib::client 永远发 HTTP/1.1（version(11) 写死），所以 HTTP/1.0 这类
    /// "只有裸报文才能构造"的请求没法用 client 覆盖，只能走这条路。
    ///
    /// 靠 EOF 定界：HTTP/1.0 默认关连接；HTTP/1.1 的调用方必须在请求里带
    /// "Connection: close"，否则对端 keep-alive 不关连接，这里会一直挂着。
    inline std::string
    raw_request(httplib::tcp::endpoint ep, std::string_view request)
    {
        net::io_context ioc;
        std::string response;
        boost::system::error_code result_ec;

        net::co_spawn(
            ioc,
            [&]() -> net::awaitable<void> {
                net::ip::tcp::socket sock{ioc};
                boost::system::error_code ec;

                sock.connect(ep, ec);
                if (ec)
                {
                    result_ec = ec;
                    co_return;
                }

                net::write(sock, net::buffer(request), ec);
                if (ec)
                {
                    result_ec = ec;
                    co_return;
                }

                char buf[4096];
                for (;;)
                {
                    auto n = sock.read_some(net::buffer(buf), ec);
                    if (ec)
                    {
                        // 对端关闭（EOF）是预期结局，不算错误。
                        break;
                    }
                    response.append(buf, n);
                }
            },
            [result_ec](std::exception_ptr e) {
                if (e)
                {
                    std::rethrow_exception(e);
                }
            });

        ioc.run();
        REQUIRE_FALSE(result_ec);
        return response;
    }

    /// 从原始响应字节里取某个 header 的值（找不到返回空串）。
    inline std::string
    raw_header(std::string_view response, std::string_view name)
    {
        auto pos = response.find("\r\n\r\n");
        auto head = (pos == std::string_view::npos) ? response : response.substr(0, pos);
        std::string needle;
        needle.reserve(name.size() + 1);
        needle.append(name).append(":");

        auto line = head.find(needle);
        if (line == std::string_view::npos)
        {
            return {};
        }
        line += needle.size();
        auto eol = head.find("\r\n", line);
        if (eol == std::string_view::npos)
        {
            eol = head.size();
        }
        auto value = head.substr(line, eol - line);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        {
            value.remove_prefix(1);
        }
        return std::string(value);
    }

    /// 响应首行里的 HTTP 版本号，例如 "HTTP/1.0 200 OK" -> 10、"HTTP/1.1 200 OK" -> 11。
    inline int
    raw_version(std::string_view response)
    {
        if (response.size() < 8 || response.compare(0, 5, "HTTP/") != 0)
        {
            return 0;
        }
        if (!std::isdigit(static_cast<unsigned char>(response[5])) || response[6] != '.' ||
            !std::isdigit(static_cast<unsigned char>(response[7])))
        {
            return 0;
        }
        return (response[5] - '0') * 10 + (response[7] - '0');
    }

    template <typename Setup, typename Test>
    void
    run(Setup&& setup, Test&& test)
    {
        net::thread_pool pool{ 1 };
        std::exception_ptr err;
        net::co_spawn(
            pool.get_executor(),
            [&]() -> net::awaitable<void>
            {
                httplib::server::http_server server(pool.get_executor());
                setup_logger(server);
                setup(server);
                server.listen("127.0.0.1", 0);
                auto ep = server.local_endpoint();
                server.run();

                httplib::client::http_client client(pool.get_executor(),
                                                     ep.address().to_string(),
                                                     ep.port());
                client.set_timeout(std::chrono::seconds(5));

                co_await test(client);

                client.close();
                server.stop();
            },
            [&](std::exception_ptr e)
            {
                err = e;
            });
        pool.join();
        if (err)
            std::rethrow_exception(err);
    }

    template <typename Test>
    void
    run_error(Test&& test)
    {
        net::thread_pool pool{ 1 };
        std::exception_ptr err;
        net::co_spawn(
            pool.get_executor(),
            [&]() -> net::awaitable<void>
            {
                co_await test(pool);
            },
            [&](std::exception_ptr e)
            {
                err = e;
            });
        pool.join();
        if (err)
            std::rethrow_exception(err);
    }

    struct test_scaffold
    {
        net::thread_pool workers_ { 4 };
        httplib::server::http_server server { workers_.get_executor() };
        httplib::tcp::endpoint endpoint;
        std::unique_ptr<httplib::client::http_client> client;

        test_scaffold()
        {
            auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            auto lg = std::make_shared<spdlog::logger>("httplib.tests", sink);
            lg->set_level(spdlog::level::debug);
            server.set_logger(lg);
        }

        ~test_scaffold()
        {
            if (client)
            {
                client->close();
            }
            server.stop();
            workers_.join();
        }
        void
        join()
        {
            workers_.join();
        }
        void
        start()
        {
            server.listen("127.0.0.1", 0);
            endpoint = server.local_endpoint();
            server.run();

            client = std::make_unique<httplib::client::http_client>(workers_.get_executor(),
                                                                    endpoint.address().to_string(),
                                                                    endpoint.port());
            client->set_timeout(std::chrono::seconds(5));
        }

        void
        start_with_long_timeout()
        {
            server.listen("127.0.0.1", 0);
            endpoint = server.local_endpoint();
            server.run();

            client = std::make_unique<httplib::client::http_client>(workers_.get_executor(),
                                                                    endpoint.address().to_string(),
                                                                    endpoint.port());
            client->set_timeout(std::chrono::seconds(30));
        }

#ifdef HTTPLIB_ENABLED_SSL

        void
        start_ssl()
        {
            server.set_ssl(kTestCert, kTestKey, "test");
            server.listen("127.0.0.1", 0);
            endpoint = server.local_endpoint();
            server.run();
            ssl_ = true;

            client = std::make_unique<httplib::client::http_client>(workers_.get_executor(),
                                                                    "localhost",
                                                                    endpoint.port(),
                                                                    httplib::url::scheme::tls);
            client->set_timeout(std::chrono::seconds(5));
        }

#endif

        auto
        executor()
        {
            return workers_.get_executor();
        }

        std::unique_ptr<httplib::client::http_client>
        make_client()
        {
            auto c = std::make_unique<httplib::client::http_client>(
                workers_.get_executor(),
                std::format("{}://{}:{}", ssl_ ? "https" : "http", ssl_ ? "localhost" : endpoint.address().to_string(), endpoint.port()));
            c->set_timeout(std::chrono::seconds(5));
            return c;
        }

        auto&
        router()
        {
            return server.router();
        }

      private:
        bool ssl_ = false;
    };

    template <typename R>
    std::string
    as_string(R const& resp)
    {
        return resp.as_string();
    }

} // namespace test_common

inline net::awaitable<void>
collect_sse_events(httplib::client::sse_reader& sse, std::vector<httplib::client::sse_reader::sse_event>& events)
{
    while (!sse.is_done())
    {
        auto result = co_await sse.read_event();
        if (result.has_error())
        {
            break;
        }
        auto& ev = result.value();
        if (ev.data.empty() && ev.id.empty() && ev.event.empty() && ev.retry == std::chrono::milliseconds { 0 })
        {
            break;
        }
        events.push_back(std::move(ev));
    }
}

inline net::awaitable<void>
collect_ndjson_lines(httplib::client::ndjson_reader& ndjson, std::vector<boost::json::value>& items)
{
    while (!ndjson.is_done())
    {
        auto result = co_await ndjson.read();
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
}

#define UNWRAP(result)               \
    [&](auto&& r)                    \
    {                                \
        REQUIRE(r.has_value());      \
        return std::move(r).value(); \
    }(result)
