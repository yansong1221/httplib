#include "request_sender.hpp"
#include "beast_alias.hpp"
#include "client/redirect_util.hpp"
#include "http_header_util.hpp"
#include "httplib/client/client.hpp"
#include "httplib/client/lazy_request.hpp"
#include <cassert>

namespace httplib::client
{

    request_sender::request_sender(std::shared_ptr<http_client_pool> pool,
                                   httplib::headers base_headers,
                                   config cfg)
        : pool_(std::move(pool))
        , base_headers_(std::move(base_headers))
        , cfg_(cfg)
    {
    }

    net::awaitable<request_sender::result>
    request_sender::send(url::url_info const& ui,
                         httplib::method m,
                         httplib::headers const& extra_headers,
                         std::atomic<bool> const& cancelled,
                         std::uint64_t per_connection_rate)
    {
        httplib::headers merged = base_headers_;
        for (auto const& f : extra_headers.fields())
        {
            merged.set(f.name_string(), f.value());
        }
        // 下载场景保存原始字节（断点续传/分片合并依赖），不接受内容编码压缩。
        merged.set(field::accept_encoding, "identity");

        auto h = ui.host;
        auto p = ui.effective_port();
        auto s = ui.transport();
        auto t = ui.target(false);
        if (t.empty())
        {
            t = "/";
        }

        for (int redir = 0; redir <= cfg_.max_redirects; ++redir)
        {
            if (cancelled.load(std::memory_order_relaxed))
            {
                result rr;
                rr.error = boost::asio::error::operation_aborted;
                co_return rr;
            }

            assert(pool_);
            auto handle = co_await pool_->async_acquire(h, p, s, cfg_.acquire_timeout);
            if (!handle)
            {
                result rr;
                rr.error = handle.error();
                co_return rr;
            }
            handle->set_timeout(cfg_.timeout);
            handle->set_max_redirects(0);
            handle->set_verify_ssl(cfg_.verify_ssl);
            handle->set_download_rate_limit(per_connection_rate);

            auto req = httplib::client::request(m, t, merged);

            auto resp_result = co_await handle->async_send_request(req, http_client::body_mode::lazy);
            if (!resp_result.has_value())
            {
                result rr;
                rr.error = resp_result.error();
                co_return rr;
            }
            auto resp = std::move(resp_result).value();

            auto status = resp.result();

            if ((status == status::moved_permanently || status == status::found || status == status::see_other
                 || status == status::temporary_redirect || status == status::permanent_redirect)
                && redir < cfg_.max_redirects)
            {
                auto rt = http_header_util::parse_redirect(resp.headers());
                if (rt.has_value())
                {
                    // Drain the redirect body before the handle returns to the
                    // pool; otherwise the leftover bytes corrupt the next
                    // request that reuses this connection. A failed drain means
                    // the connection is unusable, so close it explicitly.
                    if (auto drain_ec = co_await resp.read_body(); drain_ec)
                    {
                        handle->close();
                    }

                    if (!rt->host.empty())
                    {
                        // CL-02: 仅当 Location 为绝对 URL 且 origin 变化时移除敏感头，避免凭据泄露；
                        // 同 origin 重定向保留原头。
                        if (rt->host != h || rt->port != p || rt->transport() != s)
                        {
                            redirect::strip_origin_bound_headers(merged);
                        }
                        h = rt->host;
                        p = rt->port == 0 ? p : rt->port;
                        s = rt->transport();
                        t = rt->path.empty() ? "/" : rt->path;
                    }
                    else
                    {
                        // Relative reference: resolve against the current target so
                        // Location values like "final" or "../a/b" work correctly.
                        t = url::resolve(t, rt->path);
                    }
                    continue;
                }
            }

            result rr;
            rr.handle = std::move(handle);
            rr.response = std::move(resp);
            // 必须深拷贝：headers() 返回 rr.response 的借用视图，rr 随协程帧销毁后
            // 调用方拿到的就是悬垂指针。
            rr.headers.merge(rr.response.headers());
            rr.status = status;
            rr.final_ui = url::url_info { std::string(url::to_string(s)), h, p, t, {}, {} };
            co_return rr;
        }

        result rr;
        rr.error = boost::system::errc::make_error_code(boost::system::errc::protocol_error);
        co_return rr;
    }

} // namespace httplib::client
