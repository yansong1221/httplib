#pragma once
#include "httplib/config.hpp"
#include "httplib/html/form_data.hpp"
#include "httplib/server/proxy_interceptor.hpp"
#include "httplib/server/proxy_strategy.hpp"
#include "httplib/server/server_fwd.hpp"
#include "httplib/server/ws_interceptor.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http/fields.hpp>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace httplib::server
{
    /**
     * @brief 基于 Boost.Asio 的异步 HTTP 服务器。
     *
     * 服务器异步接受连接，并将每个请求分派给用户注册的 handler。每个被
     * 接受的连接都绑定到独立的 strand，因此不同连接可以并发推进，而同一
     * 连接内部的状态保持串行。
     *
     * 一个服务器分为两个阶段：配置阶段与运行阶段。
     * @ref run "run()" / @ref async_run "async_run()" 使服务器进入运行
     * 阶段；修改 acceptor 或路由表的成员属于配置阶段，不与在途请求做同步。
     *
     * @par 线程安全
     * @li <b>配置阶段</b>。以下成员必须在服务器启动前、由同一线程串行调用：
     *     - @ref http_server::listen "listen()"
     *     - @ref http_server::router "router()"
     *     - @ref http_server::set_reverse_proxy "set_reverse_proxy()"
     *     - @ref http_server::set_ws_forward "set_ws_forward()"
     *     - @ref http_server::set_ssl "set_ssl()"
     *     这些成员会修改 acceptor 或路由表，在服务器运行期间调用不安全。
     * @li <b>运行阶段</b>。以下成员可随时从任意线程调用，包括服务器运行
     *     期间。它们内部使用原子量或只读缓存，不影响在途连接：
     *     - @ref http_server::get_executor "get_executor()"
     *     - @ref http_server::local_endpoint "local_endpoint()"
     *     - @ref http_server::read_timeout "read_timeout()" / @ref
     *       http_server::set_read_timeout "set_read_timeout()"
     *     - @ref http_server::write_timeout "write_timeout()" / @ref
     *       http_server::set_write_timeout "set_write_timeout()"
     *     - @ref http_server::logger "logger()" / @ref http_server::set_logger
     *       "set_logger()"
     *     - @ref http_server::set_compress_content_types
     *       "set_compress_content_types()"
     *     - @ref http_server::set_form_data_config "set_form_data_config()"
     *     - @ref http_server::set_header_limit "set_header_limit()" / @ref
     *       http_server::set_body_limit "set_body_limit()"
     *     - @ref http_server::stop "stop()" / @ref http_server::async_stop
     *       "async_stop()"
     * @li <b>生命周期</b>。每个实例只允许一个运行周期。运行期间再次调用
     *     @ref run "run()" 会返回 @c already_started 错误码。@ref stop
     *     "stop()" 与 @ref async_stop "async_stop()" 可从任意线程调用，
     *     其返回的 future 在服务器完全停止、所有会话排空后变为就绪。
     */
    class HTTPLIB_API http_server
    {
      public:
        class impl;

        using compress_predicate = std::function<bool(std::string_view)>;
        using proxy_interceptor_factory = std::function<std::shared_ptr<proxy_interceptor>(request& req)>;
        using ws_interceptor_factory = std::function<std::shared_ptr<ws_interceptor>(request& req)>;

      public:
        /**
         * @brief 构造一个绑定到 I/O 执行上下文的 HTTP 服务器。
         *
         * @param ioc 服务器默认用于其异步操作的执行上下文。
         */
        explicit http_server(net::io_context& ioc);
        /**
         * @brief 构造一个绑定到 I/O 执行器的 HTTP 服务器。
         *
         * @param ex 服务器用于其异步操作的执行器。
         */
        explicit http_server(net::any_io_executor const& ex);
        /**
         * @brief 销毁 HTTP 服务器。
         *
         * 若服务器仍在运行，析构会请求停止并等待其完成。
         */
        ~http_server();

        /**
         * @brief 绑定到本地端点并开始监听连接。
         *
         * @param host 要绑定的地址，例如 @c "127.0.0.1"。
         * @param port 要绑定的端口。端口为 0 时请求内核分配一个临时端口，
         * 之后可通过 @ref local_endpoint "local_endpoint()" 查询。
         *
         * @return 指向本服务器的引用。
         *
         * @par 线程安全
         * 配置阶段。必须在服务器启动前调用。
         */
        http_server& listen(std::string_view host, uint16_t port);
        /**
         * @brief 绑定到全部接口并开始监听连接。
         *
         * 等价于以 @c "0.0.0.0" 作为 host 的 @ref listen(std::string_view
         * "listen()", uint16_t)。
         *
         * @par 线程安全
         * 配置阶段。必须在服务器启动前调用。
         */
        http_server& listen(uint16_t port);

        /**
         * @brief 启动服务器并阻塞直至其停止。
         *
         * 服务器在关联的执行器上运行。本成员不会执行任何 handler；需要有一
         * 个线程在运行底层的执行上下文。
         *
         * @return 一个 future，在服务器停止后变为就绪，其中携带结果错误码。
         * 若服务器已在运行，该 future 解析为 @c already_started 错误码。
         *
         * @par 线程安全
         * 可从任意线程调用；每个实例只允许一个运行周期。
         */
        std::future<boost::system::error_code> run();
        /**
         * @brief 启动服务器，不阻塞。
         *
         * 等价于 @ref run "run()"，但返回一个可被任意线程 co_await 的
         * awaitable；结果错误码经 @p ec 输出而非作为返回值。
         *
         * @param ec 输出参数。若服务器已在运行，写入 @c already_started；若
         *           某个监听（accept）循环以非 @c operation_aborted 的错误
         *           结束，写入该错误。正常运行停止时不会改写此值，调用前请
         *           先清零。
         *
         * @par 线程安全
         * 可从任意线程 co_await；每个实例只允许一个运行周期。
         */
        net::awaitable<void> async_run(boost::system::error_code& ec);

        /**
         * @brief 请求服务器停止。
         *
         * 停止接受新连接，并中止所有在途会话。
         *
         * @return 一个 future，在服务器完全停止、所有会话排空后变为就绪。
         *
         * @par 线程安全
         * 可随时从任意线程调用。
         */
        std::future<void> stop();
        /**
         * @brief 请求服务器停止，不阻塞。
         *
         * 等价于 @ref stop "stop()"，但返回一个可被任意线程 co_await 的
         * awaitable。
         *
         * @return 一个 awaitable，在服务器完全停止、所有会话排空后完成。
         *
         * @par 线程安全
         * 可随时从任意线程 co_await。
         */
        net::awaitable<void> async_stop();

        /**
         * @brief 获取用于注册 handler 与中间件的 router。
         *
         * @return 指向服务器 router 的引用。
         *
         * @par 线程安全
         * 配置阶段。路由注册不与在途请求同步，必须在服务器启动前完成。
         */
        httplib::server::router& router();

        /**
         * @brief 获取服务器正在监听的端点。
         *
         * 该值在 @ref listen "listen()" 调用时缓存，之后不再变化；可用于在
         * 绑定端口 0 后查询真实端口。
         *
         * @return 指向缓存的本地端点的引用。
         *
         * @par 线程安全
         * 可随时从任意线程调用。
         */
        tcp::endpoint const& local_endpoint() const;

        /**
         * @brief 设置应用于在途连接的读超时。
         *
         * @par 线程安全
         * 可随时从任意线程调用；对后续读取立即生效。
         */
        void set_read_timeout(std::chrono::steady_clock::duration const& dur);
        /**
         * @brief 设置应用于在途连接的写超时。
         *
         * @par 线程安全
         * 可随时从任意线程调用；对后续写入立即生效。
         */
        void set_write_timeout(std::chrono::steady_clock::duration const& dur);

        /**
         * @brief 获取配置的读超时。
         *
         * @par 线程安全
         * 可随时从任意线程调用。
         */
        std::chrono::steady_clock::duration read_timeout() const;
        /**
         * @brief 获取配置的写超时。
         *
         * @par 线程安全
         * 可随时从任意线程调用。
         */
        std::chrono::steady_clock::duration write_timeout() const;

        /**
         * @brief 获取服务器使用的 logger。
         *
         * @par 线程安全
         * 可随时从任意线程调用。
         */
        std::shared_ptr<spdlog::logger> logger() const;
        /**
         * @brief 替换服务器使用的 logger。
         *
         * @par 线程安全
         * 可随时从任意线程调用；对后续日志输出立即生效。
         */
        void set_logger(std::shared_ptr<spdlog::logger> logger);

        /**
         * @brief 替换用于判断某内容类型是否可压缩的谓词。
         *
         * @par 线程安全
         * 可随时从任意线程调用；对后续响应立即生效。
         */
        void set_compress_content_types(compress_predicate predicate);

        /**
         * @brief 替换 multipart 表单数据的解析配置。
         *
         * @param params 表单数据参数，例如保存目录、最大文件大小与最大字段
         * 数。
         *
         * @par 线程安全
         * 可随时从任意线程调用；对后续请求立即生效。
         */
        void set_form_data_config(html::form_data::param const& params);

        /**
         * @brief 设置允许的最大 header 大小。
         *
         * @par 线程安全
         * 可随时从任意线程调用；对后续请求生效。
         */
        void set_header_limit(std::uint32_t limit);
        /**
         * @brief 设置允许的最大 body 大小。
         *
         * @par 线程安全
         * 可随时从任意线程调用；对后续请求生效。
         */
        void set_body_limit(std::uint64_t limit);

        /**
         * @brief 将 location 前缀下的请求反向代理到固定的上游 URL。
         *
         * @par 线程安全
         * 配置阶段。必须在服务器启动前调用。
         */
        void set_reverse_proxy(std::string_view location,
                               std::string_view url,
                               proxy_interceptor_factory factory = nullptr);
        /**
         * @brief 将 location 前缀下的请求反向代理，上游由 provider 动态提供。
         *
         * @par 线程安全
         * 配置阶段。必须在服务器启动前调用。
         */
        void set_reverse_proxy(std::string_view location,
                               std::shared_ptr<upstream_provider> provider,
                               proxy_interceptor_factory factory = nullptr);
        /**
         * @brief 将 location 前缀下的请求反向代理到一组后端，按 locator 选择。
         *
         * @par 线程安全
         * 配置阶段。必须在服务器启动前调用。
         */
        void set_reverse_proxy(std::string_view location,
                               std::vector<upstream_backend> backends,
                               upstream_locator locator = upstream_locator::round_robin,
                               proxy_interceptor_factory factory = nullptr);

        /**
         * @brief 将 location 前缀下的 WebSocket 连接转发到固定的上游 URL。
         *
         * @par 线程安全
         * 配置阶段。必须在服务器启动前调用。
         */
        void set_ws_forward(std::string_view location, std::string_view url, ws_interceptor_factory factory = nullptr);
        /**
         * @brief 将 location 前缀下的 WebSocket 连接转发，上游由 provider
         * 动态提供。
         *
         * @par 线程安全
         * 配置阶段。必须在服务器启动前调用。
         */
        void set_ws_forward(std::string_view location,
                            std::shared_ptr<upstream_provider> provider,
                            ws_interceptor_factory factory = nullptr);
        /**
         * @brief 将 location 前缀下的 WebSocket 连接转发到一组后端，按
         * locator 选择。
         *
         * @par 线程安全
         * 配置阶段。必须在服务器启动前调用。
         */
        void set_ws_forward(std::string_view location,
                            std::vector<upstream_backend> backends,
                            upstream_locator locator = upstream_locator::round_robin,
                            ws_interceptor_factory factory = nullptr);

        /**
         * @brief 使用给定的证书与私钥缓冲区启用 HTTPS/WSS。
         *
         * 需要库以 @c HTTPLIB_ENABLED_SSL 编译。
         *
         * @throws boost::system::system_error 若库未启用 SSL 支持，或证书/
         * 私钥无法加载。
         *
         * @par 线程安全
         * 配置阶段。必须在服务器启动前调用。
         */
        void set_ssl(std::span<char const> const& cert_file,
                     std::span<char const> const& key_file,
                     std::string passwd = {});
        /**
         * @brief 使用指定路径的证书与私钥文件启用 HTTPS/WSS。
         *
         * 需要库以 @c HTTPLIB_ENABLED_SSL 编译。
         *
         * @throws boost::system::system_error 若库未启用 SSL 支持，或证书/
         * 私钥无法加载。
         *
         * @par 线程安全
         * 配置阶段。必须在服务器启动前调用。
         */
        void set_ssl_file(fs::path const& cert_file, fs::path const& key_file, std::string passwd = {});

      private:
        http_server(http_server const&) = delete;
        http_server& operator=(http_server const&) = delete;

        std::shared_ptr<impl> impl_;
    };

} // namespace httplib::server