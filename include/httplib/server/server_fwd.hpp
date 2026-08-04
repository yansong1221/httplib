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

    class chunk_reader;
    class chunk_writer;
    class sse_writer;
    class ndjson_writer;

} // namespace httplib::server
