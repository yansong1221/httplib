#pragma once
#include "body/body_writer.hpp"
#include "httplib/client/client.hpp"
#include "httplib/client/request.hpp"
#include <string_view>

namespace httplib::client
{
    class request::impl : public httplib::detail::body_writer<true, http_client::impl>
    {
      public:
        using body_writer_t = httplib::detail::body_writer<true, http_client::impl>;

        impl(http::verb method, std::string_view target, unsigned version) : body_writer_t()
        {
            this->base().method(method);
            this->base().target(target);
            this->base().version(version);
        }
    };
} // namespace httplib::client
