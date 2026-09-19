#pragma once
#include "httplib/config.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/server.hpp"
#include "httplib/server/server_fwd.hpp"
#include "httplib/util/use_awaitable.hpp"
#include "stream/http_stream.hpp"
#include "stream/websocket_stream.hpp"
#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/read.hpp>
#include <cstdint>
#include <memory>

namespace httplib::server
{

    class websocket_conn_impl;

    class session : public std::enable_shared_from_this<session>
    {
      public:
        class task : public std::enable_shared_from_this<task>
        {
          public:
            using ptr = std::shared_ptr<task>;

            virtual ~task() = default;
            virtual net::awaitable<task::ptr> then() = 0;
            virtual void abort() = 0;
        };
        class detect_ssl_task;
        class ssl_handshake_task;
        class http_task;
        class http_proxy_task;
        class websocket_task;

        explicit session(net::any_io_executor ex, tcp::socket&& stream, std::shared_ptr<http_server::impl> server_impl);
        ~session();

      public:
        void abort();
        net::awaitable<void> run();

        /// 本连接绑定的 strand executor；连接的启动与中止都投递到它上面执行。
        net::any_io_executor
        executor() const noexcept
        {
            return executor_;
        }

      private:
        net::any_io_executor executor_;
        /// 仅在 executor_（strand）上访问，因此无需原子；abort_ 可能来自任意线程。
        task::ptr task_;
        std::atomic_bool abort_ = false;
    };

    class session::detect_ssl_task : public session::task
    {
      public:
        explicit detect_ssl_task(tcp::socket&& stream, std::shared_ptr<http_server::impl> server_impl);
        ~detect_ssl_task();

      public:
        void abort() override;
        net::awaitable<task::ptr> then() override;

      private:
        std::shared_ptr<http_server::impl> server_impl_;
        http_stream::plain_stream stream_;
    };

    class session::http_task : public session::task
    {
      public:
        explicit http_task(http_stream&& stream,
                           beast::flat_buffer&& buffer,
                           std::shared_ptr<http_server::impl> server_impl);

        net::awaitable<task::ptr> then() override;
        void abort() override;

        // ---- 连接级读取接口（对照 client 的 http_client::impl::async_read_some）----
        // http_task 拥有 stream_/buffer_ 与本连接的读取超时，是本连接唯一的读取者；
        // request::impl 的 lazy body 读取与 header/body 解析都经由这里完成。
        template <typename Parser>
        net::awaitable<void>
        read_header(Parser& parser, boost::system::error_code& ec)
        {
            stream_.expires_after(read_timeout());
            co_await http::async_read_header(stream_, buffer_, parser, util::net_awaitable[ec]);
            stream_.expires_never();
        }

        template <typename Parser>
        net::awaitable<void>
        read_some(Parser& parser, boost::system::error_code& ec)
        {
            stream_.expires_after(read_timeout());
            co_await http::async_read_some(stream_, buffer_, parser, util::net_awaitable[ec]);
            stream_.expires_never();
        }

        // ---- 连接级写入接口（与读取接口对称，内部统一带写超时）----
        // response 的 stream_writer 与 session 的全量回写都经由这里调用。
        template <typename Serializer>
        net::awaitable<void>
        write_header(Serializer& sr, boost::system::error_code& ec)
        {
            stream_.expires_after(write_timeout());
            co_await http::async_write_header(stream_, sr, util::net_awaitable[ec]);
            stream_.expires_never();
        }
        template <typename Serializer>
        net::awaitable<void>
        write_some(Serializer& sr, boost::system::error_code& ec)
        {
            stream_.expires_after(write_timeout());
            co_await http::async_write_some(stream_, sr, util::net_awaitable[ec]);
            stream_.expires_never();
        }
        template <typename Serializer>
        net::awaitable<void>
        write(Serializer& sr, boost::system::error_code& ec)
        {
            stream_.expires_after(write_timeout());
            co_await http::async_write(stream_, sr, util::net_awaitable[ec]);
            stream_.expires_never();
        }

        // 读取/写入配置由本连接统一提供（request 的 reader_ / response 的 task_ 均经此取值）。
        std::uint64_t body_limit() const;
        html::form_data::param form_data_params() const;

        net::any_io_executor
        executor()
        {
            return stream_.get_executor();
        }

      private:
        // 超时配置仅供以上模板内部使用（server_impl_->xxx 定义见 session.cpp）。
        std::chrono::steady_clock::duration read_timeout() const;
        std::chrono::steady_clock::duration write_timeout() const;

      private:
        net::awaitable<bool> async_write(request const& req, response& resp);

      private:
        std::shared_ptr<http_server::impl> server_impl_;

        http_stream stream_;
        beast::flat_buffer buffer_;
    };

    class session::websocket_task : public session::task
    {
      public:
        explicit websocket_task(websocket_stream&& stream,
                                request&& req,
                                std::shared_ptr<http_server::impl> server_impl);

      public:
        net::awaitable<task::ptr> then() override;
        void abort() override;

      private:
        std::shared_ptr<websocket_conn_impl> conn_;
    };

    class session::http_proxy_task : public session::task
    {
      public:
        explicit http_proxy_task(http_stream&& stream, request&& req, std::shared_ptr<http_server::impl> server_impl);

      public:
        net::awaitable<task::ptr> then() override;
        void abort() override;

      private:
        http_stream stream_;
        tcp::resolver resolver_;
        tcp::socket proxy_socket_;

        request req_;
        std::shared_ptr<http_server::impl> server_impl_;
    };

} // namespace httplib::server