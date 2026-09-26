#pragma once
#include "body/body_writer.hpp"
#include "client_impl.h"
#include "httplib/client/request.hpp"
#include <boost/algorithm/string/join.hpp>
#include <boost/beast/version.hpp>
#include <string_view>

namespace httplib::client
{
    class request::impl : public httplib::detail::body_writer<true, http_client::impl>
    {
      public:
        using body_writer_t = httplib::detail::body_writer<true, http_client::impl>;

        impl(http::verb method, std::string_view target) : body_writer_t()
        {
            this->base().method(method);
            this->base().target(target);
            this->base().version(11);

            this->base().set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
            this->base().set(http::field::accept, "*/*");

            auto const& encoding = compress::compressor_factory::instance().supported_encoding();
            if (!encoding.empty())
            {
                this->base().set(http::field::accept_encoding, boost::join(encoding, ","));
            }
            this->keep_alive(true);
        }
    };
} // namespace httplib::client
