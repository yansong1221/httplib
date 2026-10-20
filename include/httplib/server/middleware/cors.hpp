#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace httplib::server::middleware
{

    class HTTPLIB_API cors_middleware
    {
      public:
        cors_middleware();
        ~cors_middleware();
        cors_middleware(cors_middleware const& other);
        cors_middleware& operator=(cors_middleware const& other);
        cors_middleware(cors_middleware&&) noexcept;
        cors_middleware& operator=(cors_middleware&&) noexcept;

        /// \brief 设置允许的 origin（覆盖式）。列表含 "*" 表示通配：无凭据时响应
        ///        Access-Control-Allow-Origin: *，凭据开启时改为回显请求 Origin
        ///        （"*" 与 Access-Control-Allow-Credentials:true 不同屏）。
        cors_middleware& allow_origins(std::vector<std::string> origins);

        /// \brief 设置单个允许的 origin（等价 allow_origins({origin})）。
        cors_middleware& allow_origin(std::string origin);
        cors_middleware& allow_methods(std::vector<std::string> methods);
        cors_middleware& allow_headers(std::vector<std::string> headers);

        /// \brief 允许携带凭据的跨源访问（Access-Control-Allow-Credentials: true）。
        cors_middleware& allow_credentials(bool allow);

        /// \brief 预检响应缓存时长（秒），>0 时写入 Access-Control-Max-Age。
        cors_middleware& max_age(int seconds);

        /// \brief 请求携带 Origin 时按白名单语义回写 CORS 头：未命中则不发送该头；
        ///        凭据开启时的通配配置改为回显请求 Origin；无 Origin 的请求按配置
        ///        原样回应（通配→*，显式单值→该值）。携带 Method 的 OPTIONS 视作
        ///        预检并以 204 短接。
        bool before(request& req, response& resp);
        bool after(request& req, response& resp);

      private:
        class impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server::middleware
