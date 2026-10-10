#include "cache_manager.hpp"
#include "http_header_util.hpp"
#include <algorithm>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

namespace httplib::client
{
    namespace detail
    {
        // Cache key = canonical URL plus the credential scope, so entries never
        // leak across distinct credentials.
        static std::string
        make_key(url::url_info const& ui, httplib::headers const& headers)
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

        // Explicit whitelist of HTTP bookkeeping the downloader needs; the rest
        // (hop-by-hop headers, partial-response framing) is discarded.
        static cache_manager::http_meta
        make_meta(httplib::headers const& response)
        {
            cache_manager::http_meta meta;
            auto take = [&](field f) -> std::string { return std::string(response[f]); };
            meta.content_type = take(field::content_type);
            meta.content_disposition = take(field::content_disposition);
            meta.etag = take(field::etag);
            meta.last_modified = take(field::last_modified);

            std::string cache_control = take(field::cache_control);
            meta.must_revalidate = http_header_util::header_has_token(cache_control, "no-cache");

            if (auto max_age = http_header_util::header_directive_int(cache_control, "max-age");
                max_age && *max_age >= 0)
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

        static std::string
        serialize_meta(cache_manager::http_meta const& meta)
        {
            boost::json::object obj;
            auto put = [&](char const* key, std::string const& value)
            {
                if (!value.empty())
                {
                    obj[key] = value;
                }
            };
            put("etag", meta.etag);
            put("last_modified", meta.last_modified);
            put("content_type", meta.content_type);
            put("content_disposition", meta.content_disposition);
            if (meta.fresh_until.has_value())
            {
                auto secs = std::chrono::duration_cast<std::chrono::seconds>(meta.fresh_until->time_since_epoch());
                obj["fresh_until"] = secs.count();
            }
            if (meta.must_revalidate)
            {
                obj["must_revalidate"] = true;
            }
            return boost::json::serialize(obj);
        }

        static std::optional<cache_manager::http_meta>
        parse_meta(std::string_view blob)
        {
            if (blob.empty())
            {
                return std::nullopt;
            }
            boost::system::error_code ec;
            auto value = boost::json::parse(blob, ec);
            if (ec || !value.is_object())
            {
                return std::nullopt;
            }
            auto const& obj = value.as_object();

            cache_manager::http_meta meta;
            auto take = [&](char const* key) -> std::string
            {
                auto v = obj.try_at(key);
                return v.has_value() && v->is_string() ? std::string(v->as_string()) : std::string {};
            };
            meta.etag = take("etag");
            meta.last_modified = take("last_modified");
            meta.content_type = take("content_type");
            meta.content_disposition = take("content_disposition");

            if (auto v = obj.try_at("fresh_until"); v.has_value() && v->is_int64())
            {
                meta.fresh_until = std::chrono::system_clock::time_point(std::chrono::seconds(v->as_int64()));
            }
            if (auto v = obj.try_at("must_revalidate"); v.has_value() && v->is_bool())
            {
                meta.must_revalidate = v->as_bool();
            }
            return meta;
        }
    } // namespace detail

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

    std::optional<cache_manager::cached_entry>
    cache_manager::get(url::url_info const& ui, httplib::headers const& request_headers) const
    {
        auto c = cache_.load(std::memory_order_acquire);
        if (!c)
        {
            return std::nullopt;
        }
        auto raw = c->get(detail::make_key(ui, request_headers));
        if (!raw)
        {
            return std::nullopt;
        }
        auto meta = detail::parse_meta(raw->metadata);
        if (!meta)
        {
            return std::nullopt;
        }
        return cached_entry { std::move(*raw), std::move(*meta) };
    }

    void
    cache_manager::put(url::url_info const& ui,
                       httplib::headers const& request_headers,
                       httplib::headers const& response_headers,
                       fs::path const& body) const
    {
        auto c = cache_.load(std::memory_order_acquire);
        if (!c)
        {
            return;
        }
        c->put(detail::make_key(ui, request_headers),
               body,
               detail::serialize_meta(detail::make_meta(response_headers)),
               std::nullopt);
    }

} // namespace httplib::client
