#pragma once
#include "httplib/config.hpp"
#include <charconv>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace httplib::html
{
    class HTTPLIB_API query_params
    {
      public:
        using container_type = std::unordered_multimap<std::string, std::string>;

        std::string_view at(std::string const& key) const;

        template <typename T = int64_t>
        T
        at_number(std::string const& key) const
        {
            auto v = at(key);

            T out {};
            auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
            if (ec != std::errc {})
            {
                throw std::runtime_error("invalid param: " + key);
            }

            return out;
        }

        bool at_bool(std::string const& key) const;

        std::vector<std::string_view> all(std::string const& key) const;

        template <typename T = int64_t>
        std::vector<T>
        all_number(std::string const& key) const
        {
            std::vector<T> result {};
            for (auto const& v : all(key))
            {
                T out {};
                auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
                if (ec != std::errc {})
                {
                    throw std::runtime_error("invalid param: " + key);
                }

                result.push_back(out);
            }

            return result;
        }
        void add(std::string const& key, std::string const& val);
        template <typename T>
        void
        add_number(std::string const& key, T const& val)
        {
            add(key, std::to_string(val));
        }
        void add_bool(std::string const& key, bool val);

        bool has(std::string const& key) const;
        bool empty() const;
        container_type const& params() const;

        bool decode(std::string_view content);
        std::string encoded() const;

      private:
        container_type params_;
    };
} // namespace httplib::html