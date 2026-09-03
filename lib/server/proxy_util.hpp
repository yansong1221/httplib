#pragma once
#include "httplib/server/request.hpp"
#include "httplib/server/server.hpp"
#include "httplib/util/misc.hpp"
#include <boost/asio/awaitable.hpp>
#include <boost/url.hpp>
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

    /// Fixed-URL proxy target backing the URL convenience overloads.
    class static_proxy_target final : public http_server::proxy_target
    {
      public:
        explicit static_proxy_target(std::string url) : url_(std::move(url)) {}
        std::string const&
        url() const override
        {
            return url_;
        }

      private:
        std::string url_;
    };

    /// Wraps a fixed upstream URL into a resolver that always yields it.
    inline http_server::proxy_resolver
    make_static_resolver(std::string url)
    {
        auto target = std::make_shared<static_proxy_target>(std::move(url));
        return [target = std::move(target)](request&) -> net::awaitable<std::shared_ptr<http_server::proxy_target>>
        { co_return target; };
    }

    /// Transport-neutral description of an upstream endpoint derived from a proxy_target URL.
    struct parsed_upstream
    {
        std::string raw_url; // the URL string returned by the resolver (for diagnostics)
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
        no_target, // resolver returned null
        bad_url    // resolver URL failed to parse
    };

    struct upstream_resolve_result
    {
        upstream_resolve_rc rc = upstream_resolve_rc::ok;
        parsed_upstream value;
    };

    /// Resolves the upstream target and parses its URL in one step.
    ///
    /// \param websocket Whether the caller speaks WebSocket semantics: affects the
    ///                  TLS detection (wss counts) and the scheme of the built URL.
    inline net::awaitable<upstream_resolve_result>
    resolve_upstream(http_server::proxy_resolver const& resolver, request& req, std::string_view prefix, bool websocket)
    {
        upstream_resolve_result out;

        auto target = co_await resolver(req);
        if (!target)
        {
            out.rc = upstream_resolve_rc::no_target;
            co_return out;
        }

        auto const& url = target->url();
        out.value.raw_url = url;

        auto r = boost::urls::parse_uri(url);
        if (!r)
        {
            out.rc = upstream_resolve_rc::bad_url;
            co_return out;
        }

        auto const& u = *r;
        auto& v = out.value;
        v.host = std::string(u.host());
        v.scheme = std::string(u.scheme());
        v.ssl = websocket ? (v.scheme == "wss" || v.scheme == "https") : (v.scheme == "https");
        v.port = u.port_number() ? u.port_number() : (v.ssl ? 443 : 80);
        v.prefix_path = std::string(u.encoded_path());
        v.target_path = make_upstream_path(req.target(), prefix, v.prefix_path);
        v.url = websocket ? util::make_url_value(v.host, v.port, v.ssl, v.target_path, v.scheme)
                          : util::make_url_value(v.host, v.port, v.ssl, v.target_path);

        co_return out;
    }

} // namespace httplib::server::detail
