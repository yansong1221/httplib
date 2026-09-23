#pragma once
#include "httplib/server/request.hpp"
#include "httplib/server/server.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/url/url.hpp"
#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace httplib::server::detail
{

    /// Strips trailing '/' and '*' from a proxy route prefix.
    inline std::string
    strip_proxy_prefix(std::string_view route)
    {
        std::string result(route);
        while (!result.empty() && (result.back() == '/' || result.back() == '*'))
        {
            result.pop_back();
        }
        return result;
    }

    /// Joins a client target path with the proxy prefix onto an upstream base path.
    inline std::string
    make_upstream_path(std::string_view client_target, std::string_view proxy_prefix, std::string_view upstream_base)
    {
        if (!client_target.starts_with(proxy_prefix))
        {
            return {};
        }

        auto tail = client_target.substr(proxy_prefix.size());

        if (tail.empty())
        {
            return upstream_base.empty() ? "/" : std::string(upstream_base);
        }

        std::string result(upstream_base);

        if (result.size() > 1 && result.ends_with('/'))
        {
            result.pop_back();
        }

        if (tail.front() != '/' && tail.front() != '?')
        {
            result += '/';
        }
        else if (result.ends_with('/') && tail.front() == '/')
        {
            tail.remove_prefix(1);
        }

        result += tail;

        return result;
    }

    /// \brief Provider that always yields a fixed upstream URL.
    class static_upstream_provider final : public upstream_provider
    {
      public:
        explicit static_upstream_provider(std::string url) : url_(std::move(url)) {}

        net::awaitable<std::string>
        url(request&) override
        {
            co_return url_;
        }

      private:
        std::string url_;
    };

    /// Transport-neutral description of an upstream endpoint derived from a provided URL.
    struct parsed_upstream
    {
        std::string raw_url; // the URL string returned by the provider (for diagnostics)
        std::string host;
        std::string scheme;
        uint16_t port = 80;
        bool ssl = false;
        std::string prefix_path; // encoded base path taken from the URL
        std::string target_path; // client target rewritten onto the upstream base path
        std::string url;         // full upstream URL, scheme-aware (http(s):// or the original scheme)
    };

    enum class upstream_resolve_rc
    {
        ok,
        no_target, // provider is null
        bad_url    // provider returned a URL that failed to parse
    };

    struct upstream_resolve_result
    {
        upstream_resolve_rc rc = upstream_resolve_rc::ok;
        parsed_upstream value;
    };

    /// Resolves the upstream URL by asking the provider and parses it in one step.
    ///
    /// \param websocket Whether the caller speaks WebSocket semantics: affects the
    ///                  TLS detection (wss counts) and the scheme of the built URL.
    inline net::awaitable<upstream_resolve_result>
    resolve_upstream(std::shared_ptr<upstream_provider> const& provider,
                     request& req,
                     std::string_view prefix,
                     bool websocket)
    {
        upstream_resolve_result out;

        if (!provider)
        {
            out.rc = upstream_resolve_rc::no_target;
            co_return out;
        }

        auto url = co_await provider->url(req);
        out.value.raw_url = url;

        auto r = url::parse_url(url);
        if (!r)
        {
            out.rc = upstream_resolve_rc::bad_url;
            co_return out;
        }

        auto const& u = *r;
        auto& v = out.value;
        v.host = u.host;
        v.scheme = u.scheme;
        v.ssl = websocket ? u.is_ssl() : (v.scheme == "https");
        v.port = u.effective_port();
        v.prefix_path = u.path;
        v.target_path = make_upstream_path(req.target(), prefix, v.prefix_path);
        v.url = websocket ? url::make_url_value(v.host, v.port, v.ssl ? url::scheme::tls : url::scheme::plain, v.target_path, v.scheme)
                          : url::make_url_value(v.host, v.port, v.ssl ? url::scheme::tls : url::scheme::plain, v.target_path);

        co_return out;
    }

} // namespace httplib::server::detail
