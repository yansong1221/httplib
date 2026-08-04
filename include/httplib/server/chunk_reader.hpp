#pragma once
#include "httplib/config.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/system/result.hpp>

namespace httplib::server
{

    class chunk_reader
    {
      public:
        virtual ~chunk_reader() = default;
        virtual net::awaitable<boost::system::result<std::size_t>> read_some(net::mutable_buffer const& buffer) = 0;
        virtual bool is_done() const = 0;
    };

} // namespace httplib::server
