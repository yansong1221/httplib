#pragma once
#include "httplib/config.hpp"
#include <memory>

namespace httplib::client
{

    class http_client;
    class http_client_pool;
    class downloader;
    class cache;
    class disk_cache;
    class read_session;
    class lazy_request;
    class request;
    class response;
    class lazy_response;
    class sse_reader;
    class ndjson_reader;

} // namespace httplib::client
