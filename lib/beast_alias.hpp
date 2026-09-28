// lib/ 私有头：Beast 的命名空间别名。
//
// 别名只放在这里，不放在公共的 include/httplib/config.hpp 里——公共面上不应该
// 出现任何指向 boost::beast 的名字，调用方想用 Beast 请自己 #include。
//
// lib/ 下所有用到 http:: / websocket:: / beast:: 的文件都需要包含本头。
// include/httplib/config.hpp 仍会前向声明 boost::beast 的空命名空间，公共头里
// 只用 net::awaitable 之类，不需要真正的 Beast 定义。
#pragma once
#include "httplib/config.hpp"

#include <boost/beast/core/role.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

namespace httplib
{
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace websocket = beast::websocket;
} // namespace httplib
