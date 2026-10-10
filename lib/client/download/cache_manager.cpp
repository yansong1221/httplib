#include "cache_manager.hpp"
#include "http_header_util.hpp"
#include <algorithm>
#include <boost/algorithm/string/trim.hpp>
#include <format>

namespace httplib::client
{
    using boost::algorithm::trim;

    void
    cache_manager::set_cache(std::shared_ptr<cache> c)
    {
        cache_.store(std::move(c), std::memory_order_release);
    }

    bool
    cache_manager::enabled() const
    {
        return cache_.load(std::memory_order_acquire) != nullptr;
    }

    std::shared_ptr<cache>
    cache_manager::raw_cache() const
    {
        return cache_.load(std::memory_order_acquire);
    }

    std::optional<cache::entry>
    cache_manager::get(std::string_view key) const
    {
        auto c = cache_.load(std::memory_order_acquire);
        if (!c)
        {
            return std::nullopt;
        }
        return c->get(key);
    }

    void
    cache_manager::put(std::string_view key, fs::path const& body, http_meta const& meta) const
    {
        auto c = cache_.load(std::memory_order_acquire);
        if (!c)
        {
            return;
        }
        c->put(key, body, serialize_meta(meta), std::nullopt);
    }

    std::string
    cache_manager::make_key(url::url_info const& ui, httplib::headers const& headers)
    {
        auto key = ui.to_url();
        auto scope = http_header_util::cache_auth_scope(headers);
        if (!scope.empty())
        {
            key.append("|auth=");
            key.append(scope);
        }
        return key;
    }

    cache_manager::http_meta
    cache_manager::make_meta(httplib::headers const& response)
    {
        // Explicit whitelist of HTTP bookkeeping the downloader needs; the rest
        // (hop-by-hop headers, partial-response framing) is discarded.
        http_meta meta;
        auto take = [&](field f) -> std::string { return std::string(response[f]); };
        meta.content_type = take(field::content_type);
        meta.content_disposition = take(field::content_disposition);
        meta.etag = take(field::etag);
        meta.last_modified = take(field::last_modified);

        std::string cache_control = take(field::cache_control);
        meta.must_revalidate = http_header_util::header_has_token(cache_control, "no-cache");

        if (auto max_age = http_header_util::header_directive_int(cache_control, "max-age"); max_age && *max_age >= 0)
        {
            std::int64_t age = 0;
            if (auto age_header = response[field::age]; !age_header.empty())
            {
                try
                {
                    age = std::stoll(std::string(age_header));
                }
                catch (...)
                {
                    age = 0;
                }
            }
            auto lifetime = std::chrono::seconds(std::max<std::int64_t>(0, *max_age - age));
            meta.fresh_until = std::chrono::system_clock::now() + lifetime;
        }
        return meta;
    }

    std::string
    cache_manager::serialize_meta(http_meta const& meta)
    {
        std::string out;
        auto put = [&](std::string_view key, std::string const& value)
        {
            if (!value.empty())
            {
                out.append(key);
                out.push_back('=');
                out.append(value);
                out.push_back('\n');
            }
        };
        put("etag", meta.etag);
        put("last_modified", meta.last_modified);
        put("content_type", meta.content_type);
        put("content_disposition", meta.content_disposition);
        if (meta.fresh_until.has_value())
        {
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(meta.fresh_until->time_since_epoch());
            out.append("fresh_until=");
            out.append(std::to_string(secs.count()));
            out.push_back('\n');
        }
        if (meta.must_revalidate)
        {
            out.append("must_revalidate=1\n");
        }
        return out;
    }

    std::optional<cache_manager::http_meta>
    cache_manager::parse_meta(std::string_view blob)
    {
        if (blob.empty())
        {
            return std::nullopt;
        }
        http_meta meta;
        std::size_t pos = 0;
        while (pos < blob.size())
        {
            auto nl = blob.find('\n', pos);
            auto end = (nl == std::string_view::npos) ? blob.size() : nl;
            std::string line(blob.substr(pos, end - pos));
            pos = (nl == std::string_view::npos) ? blob.size() : nl + 1;

            trim(line);
            if (line.empty())
            {
                continue;
            }
            auto eq = line.find('=');
            if (eq == std::string::npos)
            {
                continue;
            }
            auto key = line.substr(0, eq);
            auto val = line.substr(eq + 1);
            if (key == "etag")
            {
                meta.etag = val;
            }
            else if (key == "last_modified")
            {
                meta.last_modified = val;
            }
            else if (key == "content_type")
            {
                meta.content_type = val;
            }
            else if (key == "content_disposition")
            {
                meta.content_disposition = val;
            }
            else if (key == "fresh_until")
            {
                try
                {
                    meta.fresh_until = std::chrono::system_clock::time_point(std::chrono::seconds(std::stoll(val)));
                }
                catch (...)
                {
                }
            }
            else if (key == "must_revalidate")
            {
                meta.must_revalidate = (val == "1");
            }
        }
        return meta;
    }

} // namespace httplib::client
