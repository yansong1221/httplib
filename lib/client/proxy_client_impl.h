#pragma once
#include "beast_alias.hpp"
#include "httplib/client/proxy_client.hpp"
#include "stream/http_stream.hpp"
#include "util/logging.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <future>

namespace httplib::client
{

    class proxy_client::impl
        : public httplib::detail::logger
        , public std::enable_shared_from_this<impl>
    {
      public:
        impl(net::any_io_executor const& ex, std::string_view host, uint16_t port, url::scheme s);

      public:
        net::awaitable<void> async_connect(std::string_view target,
                                           httplib::headers const& headers,
                                           boost::system::error_code& ec);

        net::awaitable<std::size_t> async_read_some(net::mutable_buffer const& buffer, boost::system::error_code& ec);

        net::awaitable<void> async_write(net::const_buffer const& buffer, boost::system::error_code& ec);

        std::future<void> close();
        net::awaitable<void> async_close();

        bool is_open() const noexcept;

        void
        set_verify_ssl(bool verify)
        {
            verify_ssl_.store(verify);
        }
        void
        set_ca_cert(std::string_view cert)
        {
            ca_cert_.store(std::make_shared<std::string const>(cert));
        }

      private:
        std::shared_ptr<http_stream> get_stream(boost::system::error_code& ec) const;

      private:
        net::strand<net::any_io_executor> strand_;
        tcp::resolver resolver_;
        std::string const host_;
        uint16_t const port_ = 0;
        url::scheme const scheme_ = url::scheme::plain;

        // 这两项会被 strand 上的 async_connect 读取（见 proxy_client_impl.cpp），
        // 而 setter 可从任意线程调用，因此必须是原子的。std::string 不是 trivially
        // copyable，不能直接塞进 std::atomic —— 用 atomic<shared_ptr> 拿快照，
        // 引用计数同时保证被读到的缓冲区存活。与 ws_client_impl.h 一致。
        std::atomic<bool> verify_ssl_ = true;
        std::atomic<std::shared_ptr<std::string const>> ca_cert_;

        std::atomic<std::shared_ptr<http_stream>> stream_;
        beast::flat_buffer buffer_;
    };

} // namespace httplib::client
