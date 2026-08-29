#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/db/options.hpp"
#include <cctype>
#include <charconv>
#include <utility>

namespace httplib::db
{

    options
    options::parse(std::string_view s)
    {
        options o;
        size_t i = 0;
        size_t const n = s.size();
        while (i < n)
        {
            while (i < n && std::isspace(static_cast<unsigned char>(s[i])))
            {
                ++i;
            }
            if (i >= n)
            {
                break;
            }

            std::string key;
            while (i < n && s[i] != '=' && !std::isspace(static_cast<unsigned char>(s[i])))
            {
                key.push_back(s[i]);
                ++i;
            }
            if (i < n && s[i] == '=')
            {
                ++i;
            }

            std::string value;
            if (i < n && (s[i] == '"' || s[i] == '\''))
            {
                char const quote = s[i];
                ++i;
                while (i < n && s[i] != quote)
                {
                    if (s[i] == '\\' && i + 1 < n)
                    {
                        ++i;
                    }
                    value.push_back(s[i]);
                    ++i;
                }
                if (i < n)
                {
                    ++i; // 跳过闭合引号
                }
            }
            else
            {
                while (i < n && !std::isspace(static_cast<unsigned char>(s[i])))
                {
                    value.push_back(s[i]);
                    ++i;
                }
            }

            if (!key.empty())
            {
                o.set(key, std::move(value));
            }
        }
        return o;
    }

    void
    options::set(std::string_view key, std::string value)
    {
        kv_.insert_or_assign(std::string(key), std::move(value));
    }

    bool
    options::has(std::string_view key) const
    {
        return kv_.find(key) != kv_.end();
    }

    std::optional<std::string>
    options::get(std::string_view key) const
    {
        auto it = kv_.find(key);
        if (it == kv_.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    std::string
    options::get_or(std::string_view key, std::string def) const
    {
        auto it = kv_.find(key);
        return it == kv_.end() ? std::move(def) : it->second;
    }

    std::optional<uint16_t>
    options::as_uint16(std::string_view key) const
    {
        auto v = get(key);
        if (!v)
        {
            return std::nullopt;
        }
        unsigned int x = 0;
        auto r = std::from_chars(v->data(), v->data() + v->size(), x);
        if (r.ec != std::errc {} || r.ptr != v->data() + v->size() || x > 65535)
        {
            return std::nullopt;
        }
        return static_cast<uint16_t>(x);
    }

    std::optional<int>
    options::as_int(std::string_view key) const
    {
        auto v = get(key);
        if (!v)
        {
            return std::nullopt;
        }
        int x = 0;
        auto r = std::from_chars(v->data(), v->data() + v->size(), x);
        if (r.ec != std::errc {} || r.ptr != v->data() + v->size())
        {
            return std::nullopt;
        }
        return x;
    }

    std::optional<std::chrono::seconds>
    options::as_seconds(std::string_view key) const
    {
        auto v = get(key);
        if (!v)
        {
            return std::nullopt;
        }
        long long x = 0;
        auto r = std::from_chars(v->data(), v->data() + v->size(), x);
        if (r.ec != std::errc {} || r.ptr != v->data() + v->size())
        {
            return std::nullopt;
        }
        return std::chrono::seconds(x);
    }

    std::optional<bool>
    options::as_bool(std::string_view key) const
    {
        auto v = get(key);
        if (!v)
        {
            return std::nullopt;
        }
        if (*v == "1" || *v == "true" || *v == "yes" || *v == "on")
        {
            return true;
        }
        if (*v == "0" || *v == "false" || *v == "no" || *v == "off")
        {
            return false;
        }
        return std::nullopt;
    }

    std::string
    options::to_connection_string() const
    {
        std::string out;
        for (auto const& [k, v] : kv_)
        {
            if (!out.empty())
            {
                out += ' ';
            }
            out += k;
            out += '=';
            bool const need_quote = v.empty() || v.find_first_of(" \t\r\n\f\v\"'=") != std::string::npos;
            if (need_quote)
            {
                out += '"';
                for (char c : v)
                {
                    if (c == '"' || c == '\\')
                    {
                        out += '\\';
                    }
                    out += c;
                }
                out += '"';
            }
            else
            {
                out += v;
            }
        }
        return out;
    }

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
