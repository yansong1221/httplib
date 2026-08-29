#pragma once
#include "httplib/config.hpp"

namespace httplib::server
{

    class request;
    class response;
    class http_server;
    class router;
    class websocket_conn;
    class mount_point_entry;
    class session;

    class stream_writer;
    class sse_writer;
    class ndjson_writer;

    class proxy_interceptor;
    class ws_interceptor;

} // namespace httplib::server
