#pragma once
#include "httplib/server/server.hpp"
#include <boost/asio/awaitable.hpp>
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

} // namespace httplib::server::detail
