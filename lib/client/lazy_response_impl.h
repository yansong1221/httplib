#pragma once
#include "client_impl.h"
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <memory>

namespace httplib::client
{
    class lazy_response::impl : public std::enable_shared_from_this<lazy_response::impl>
    {
      public:
        explicit impl(std::unique_ptr<http::response_parser<http::empty_body>>&& header_parser,
                      std::shared_ptr<http_client::impl> parent);

        static net::awaitable<boost::system::result<client::lazy_response>> create(
            std::unique_ptr<http::response_parser<http::empty_body>>&& header_parser,
            std::shared_ptr<http_client::impl> parent);

        http::status result() const;
        http::fields const& headers() const;
        http::fields& headers();
        bool is_body_done() const;

        net::awaitable<boost::system::result<std::size_t>> read_some(net::mutable_buffer const& buf);

        net::awaitable<boost::system::result<client::response>> read_body(
            http_client::impl::body_setup_fn const& body_setup);

        net::awaitable<boost::system::result<client::response>>
        read_file(fs::path const& save_path)
        {
            body::file_body::value_type fb;
            fb.open(save_path, std::ios::out | std::ios::binary | std::ios::trunc);
            if (!fb.is_open())
            {
                co_return boost::system::errc::make_error_code(boost::system::errc::permission_denied);
            }
            co_return co_await read_body([&](http::response<body::any_body>& resp) { resp.body() = std::move(fb); });
        }

      private:
        http::status status_ { http::status::unknown };
        http::fields header_;
        std::shared_ptr<http_client::impl> parent_;
        std::unique_ptr<http::response_parser<http::empty_body>> header_parser_;
        std::unique_ptr<http::response_parser<http::buffer_body>> resp_parser_;
    };
} // namespace httplib::client
