#pragma once
#include "httplib/config.hpp"
#include <memory>

namespace httplib::client
{

    class http_client;
    class http_client_pool;
    class header_read_session;
    class read_session;
    class write_session;
    class sse_reader;
    class ndjson_reader;

} // namespace httplib::client
