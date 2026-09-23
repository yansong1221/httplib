#pragma once
#include "httplib/client/client_fwd.hpp"
#include "httplib/client/request.hpp"
#include "httplib/client/response.hpp"
#include "httplib/url/scheme.hpp"
#include <boost/asio/awaitable.hpp>
#include <filesystem>
#include <future>
#include <memory>

namespace httplib::client
{

    /**
     * \brief HTTP/1.1 客户端：一个对象对应一条底层 TCP/TLS 连接。
     * \details
     * 所有异步操作返回 awaitable，并在客户端自己的 strand（由构造传入的 executor
     * 派生的 \c net::strand）上串行执行；连接在首次发送请求时惰性建立（DNS 解析、
     * 握手、TLS 等），\ref close 之后再次发送会自动重连。非池化场景建议配合
     * \ref http_client_pool 复用连接。
     *
     * \par 单飞行约束
     * 同一时刻同一 client 上最多只允许「一个请求在途」（single-flight）。eager
     * 请求返回即该请求结束；lazy 请求必须等响应体读完（见 \ref response::is_body_done）
     * 才算结束。该约束未做防护：违反会导致同一 socket 的交错读写（未定义行为）。
     *
     * \par 线程安全
     * 不可拷贝。除标注「可任意线程调用」的配置接口外，其余接口建议在连接空闲时
     * （无在途请求、无未读完的 lazy 响应）调用。
     */
    class HTTPLIB_API http_client
    {
      public:
        /**
         * \brief 超时策略。
         * \details \c overall：整个请求（连接 + 发送 + 接收）共用一个总时限；
         * \c step：每个 I/O 步骤独立计时；\c never：不设超时。
         */
        enum class timeout_policy
        {
            overall,
            step,
            never
        };

        /**
         * \brief 响应体读取方式。
         * \details \c eager：\ref async_send_request 返回前已读完整响应体，返回后连接
         * 即可用于下一个请求。\c lazy：仅完成响应头解析即返回，响应体按需读取
         * （见 \ref response 的 \c read_* / 流式读取接口）。lazy 必须先把响应体读完
         * （或让响应对象析构——析构会关闭连接）才能发起下一个请求。
         */
        enum class body_mode
        {
            eager,
            lazy
        };
        using response_result = boost::system::result<response>;

      public:
        /// \brief 创建一个客户端。
        /// \param ex 客户端使用的 io_context 或其 executor。
        /// \param host 主机名，用于 DNS 解析与 Host 头。
        /// \param port 端口。
        /// \param s 传输方案（\ref httplib::url::scheme）：\c url::scheme::tls 走 HTTPS，默认 \c url::scheme::plain。
        explicit http_client(net::io_context& ex,
                             std::string_view host,
                             uint16_t port,
                             httplib::url::scheme s = httplib::url::scheme::plain);
        explicit http_client(net::any_io_executor const& ex,
                             std::string_view host,
                             uint16_t port,
                             httplib::url::scheme s = httplib::url::scheme::plain);

        /// \brief 从 URL 创建客户端。
        /// \param url HTTP(S) URL（如 \c https://api.example.com:8443/base ），解析出 host、port、ssl。
        explicit http_client(net::io_context& ex, std::string_view url);
        explicit http_client(net::any_io_executor const& ex, std::string_view url);

        ~http_client();

        /// \brief 设置超时策略。可任意线程调用；对在途请求仍生效（overall 模式）。
        void set_timeout_policy(timeout_policy const& policy);

        /// \brief 设置请求超时时长。可任意线程调用；在 \c overall / \c step 策略下生效。
        void set_timeout(std::chrono::steady_clock::duration const& duration);

        /// \brief 返回主机名。
        std::string_view host() const;

        /// \brief 返回端口。
        uint16_t port() const;

        /// \brief 是否使用 TLS。
        httplib::url::scheme scheme() const;

        /// \brief 返回当前日志器。
        std::shared_ptr<spdlog::logger> logger() const;

        /// \brief 设置日志器。可任意线程调用。
        void set_logger(std::shared_ptr<spdlog::logger> logger);

        /// \brief 设置最大重定向次数。可任意线程调用；`0` 表示不跟随重定向。
        void set_max_redirects(int n);

        /// \brief 是否校验服务端证书。可任意线程调用；仅 SSL 连接生效。
        void set_verify_ssl(bool verify);

        /// \brief 设置 CA 证书（PEM 格式）。可任意线程调用；仅 SSL 连接生效。
        /// \details 原子替换内部证书快照，对在途请求的后续（重连）也生效。
        void set_ca_cert(std::string_view cert);

        /// \brief 设置响应头解析上限（字节）。可任意线程调用；超出时报错关闭连接。
        void set_header_limit(std::uint32_t limit);

        /// \brief 设置响应体上限（字节）。可任意线程调用；超出时报错关闭连接。
        void set_body_limit(std::uint64_t limit);

        /// \brief Cap the response-body (download) throughput for this connection, in
        /// bytes per second. A value of 0 means unlimited. Backed by the stream's
        /// Beast read rate limit.
        ///
        /// Contract: must not be called while a request is in flight on the same
        /// client（写活连接的 rate_policy，与在途读写并发不安全）。空闲时设置，
        /// 或在对连接发起请求之前设置。
        void set_download_rate_limit(std::uint64_t bytes_per_second);

        /// \brief Cap the request-body (upload) throughput for this connection, in bytes
        /// per second. A value of 0 means unlimited. Backed by the stream's Beast
        /// write rate limit.
        ///
        /// Contract: 同 set_download_rate_limit，请求在途时调用不安全。
        void set_upload_rate_limit(std::uint64_t bytes_per_second);

        /// \brief 异步关闭底层连接（阻塞式包装）。
        /// \details 可在任意线程调用，内部把关闭动作投递到 strand 执行并向 future 就绪。
        /// 注意：在 io 线程的处理函数里对该 future \c .get() 会死锁（完成回调与当前
        /// 线程互相等待），应使用 \ref async_close。
        std::future<void> close();

        /// \brief 异步关闭底层连接的协程版本。
        net::awaitable<void> async_close();

        /// \brief 当前是否仍有已打开的底层连接。
        /// \details 连接句柄以原子快照读取；对底层 socket open 标志的探测是标量读，
        /// 与 open 状态的并发写（如 \ref async_close 中的 close()）在实践上安全（最坏
        /// 返回过期值、不会崩溃），但按 Asio 约定（shared socket: unsafe）不属于
        /// 线程安全。建议仅在连接空闲时调用。
        bool is_open() const;

        /// \brief 是否存在未结束的 lazy 会话（未发送的 lazy_request，或响应体未读完的
        /// lazy \ref response）。
        /// \details 供连接池判断连接是否已可复用；返回 \c false 才可安全发起下一个请求。
        /// 可任意线程调用。
        bool has_active_session() const;

        /// \brief 探测对端连接是否存活（TCP 层 FIN/RST 检测，MSG_PEEK）。
        /// \returns future 在连接存活时回 \c true。
        /// \details Contract：只能在连接空闲时调用（requests/读写未在途，且不与
        /// close() 并发）。探测是对底层 socket 的同步 PEEK，与在途 async I/O 并发
        /// 操作同一 socket 是不允许的。
        std::future<bool> is_alive() const;

        /// \brief \ref is_alive 的协程版本。Contract 同上：连接必须空闲。
        net::awaitable<bool> async_is_alive() const;

      public:
        /// \brief 创建一个可延迟发送的请求对象（lazy）。
        /// \details 返回的 \ref lazy_request 绑定到本 client：在其上发送时写请求体走
        /// 独立缓冲，与 \ref async_send_request 互斥。同一时刻仍只允许一个请求在途。
        std::shared_ptr<lazy_request> create_lazy_request();
        // ---- core send ----

        /// \brief 发送一个 HTTP 请求并等待响应。
        /// \param req 请求（含方法、头与请求体）。
        /// \param mode 响应体读取方式，见 \ref body_mode。
        /// \returns \ref response_result：\c response 或 error_code。
        /// \details
        /// Contract：同一 client 同一时刻最多一个请求在途（见类文档的「单飞行约束」）。
        /// \c eager 模式下返回即请求结束；\c lazy 模式下必须先读完响应体再发起下一个请求。
        /// 负责：按需连接（含重连）、模板化请求序列化、响应解析、跟随重定向
        /// （次数上限见 \ref set_max_redirects）。
        net::awaitable<response_result> async_send_request(request& req, body_mode mode = body_mode::eager);
        net::awaitable<response_result> async_send_request(request&& req, body_mode mode = body_mode::eager);
        // net::awaitable<response_result> async_send_request(request req, body_mode mode = body_mode::eager);

        // ---- HTTP method shorthands (no body) ----

        /// \brief shorthands：构造一个无请求体的请求并按 eager 发送。
        /// \param path 请求路径。
        /// \param params 附加到路径的查询参数。
        /// \param headers 附加请求头。
        net::awaitable<response_result> async_get(std::string_view path,
                                                  html::query_params const& params = {},
                                                  http::fields const& headers = http::fields());
        net::awaitable<response_result> async_head(std::string_view path,
                                                   html::query_params const& params = {},
                                                   http::fields const& headers = http::fields());
        net::awaitable<response_result> async_post(std::string_view path,
                                                   html::query_params const& params = {},
                                                   http::fields const& headers = http::fields());
        net::awaitable<response_result> async_put(std::string_view path,
                                                  html::query_params const& params = {},
                                                  http::fields const& headers = http::fields());
        net::awaitable<response_result> async_patch(std::string_view path,
                                                    html::query_params const& params = {},
                                                    http::fields const& headers = http::fields());
        net::awaitable<response_result> async_del(std::string_view path,
                                                  html::query_params const& params = {},
                                                  http::fields const& headers = http::fields());
        net::awaitable<response_result> async_options(std::string_view path,
                                                      html::query_params const& params = {},
                                                      http::fields const& headers = http::fields());

        // ---- HTTP method shorthands (with body) ----

        /// \brief shorthands：按给定请求体 + content-type 发送（eager）。
        net::awaitable<response_result> async_post(std::string_view path,
                                                   std::string_view body,
                                                   std::string_view content_type,
                                                   html::query_params const& params = {},
                                                   http::fields const& headers = http::fields());
        net::awaitable<response_result> async_post(std::string_view path,
                                                   boost::json::value&& body,
                                                   html::query_params const& params = {},
                                                   http::fields const& headers = http::fields());
        net::awaitable<response_result> async_put(std::string_view path,
                                                  std::string_view body,
                                                  std::string_view content_type,
                                                  html::query_params const& params = {},
                                                  http::fields const& headers = http::fields());
        net::awaitable<response_result> async_put(std::string_view path,
                                                  boost::json::value&& body,
                                                  html::query_params const& params = {},
                                                  http::fields const& headers = http::fields());
        net::awaitable<response_result> async_patch(std::string_view path,
                                                    std::string_view body,
                                                    std::string_view content_type,
                                                    html::query_params const& params = {},
                                                    http::fields const& headers = http::fields());
        net::awaitable<response_result> async_patch(std::string_view path,
                                                    boost::json::value&& body,
                                                    html::query_params const& params = {},
                                                    http::fields const& headers = http::fields());

        // ---- download ----

        /// \brief 流式下载到文件（自动处理分块与 content-encoding 解压）。
        /// \param method HTTP 方法（GET/HEAD/POST 等）。
        /// \param path 请求路径。
        /// \param save_path 保存路径。
        /// \param headers 附加请求头。
        net::awaitable<response_result> async_download(http::verb method,
                                                       std::string_view path,
                                                       fs::path const& save_path,
                                                       http::fields const& headers = http::fields());

      private:
        http_client(http_client const&) = delete;
        http_client& operator=(http_client const&) = delete;

        class impl;
        std::shared_ptr<impl> impl_;

        friend class ::httplib::client::response::impl;

        friend std::shared_ptr<impl>&
        get_impl(http_client& self)
        {
            return self.impl_;
        }
        friend std::shared_ptr<impl> const&
        get_impl(http_client const& self)
        {
            return self.impl_;
        }
    };

} // namespace httplib::client