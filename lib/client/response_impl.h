#pragma once
#include "client_impl.h"
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/parser.hpp>
#include <memory>

namespace httplib::client
{
    class response::impl : public std::enable_shared_from_this<response::impl>
    {
      public:
        explicit impl(std::unique_ptr<http::response_parser<http::empty_body>>&& header_parser,
                      std::shared_ptr<http_client::impl> parent);

        static net::awaitable<boost::system::result<client::response>>
        create(std::unique_ptr<http::response_parser<http::empty_body>>&& header_parser,
               std::shared_ptr<http_client::impl> parent);

        http::status result() const;
        http::fields const& headers() const;
        http::fields& headers();
        bool is_body_done() const;

        net::awaitable<boost::system::result<std::size_t>> read_some(net::mutable_buffer const& buf);

        net::awaitable<boost::system::result<http_client::response>>
        read_body(http_client::impl::body_setup_fn const& body_setup);

      private:
        http::status status_ { http::status::unknown };
        http::fields header_;
        std::shared_ptr<http_client::impl> parent_;
        std::unique_ptr<http::response_parser<http::empty_body>> header_parser_;
        std::unique_ptr<http::response_parser<http::buffer_body>> resp_parser_;
    };
} // namespace httplib::client
