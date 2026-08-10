#pragma once
#include "httplib/config.hpp"
#include "httplib/util/misc.hpp"
#include <charconv>
#include <concepts>
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

        template <typename T = std::string_view>
            requires std::integral<T> || std::floating_point<T> || std::same_as<T, bool> || std::same_as<T, std::string>
                     || std::same_as<T, std::string_view>
        T
        at(std::string const& key) const
        {
            return convert<T>(at_raw(key));
        }

        template <typename T = std::string_view>
            requires std::integral<T> || std::floating_point<T> || std::same_as<T, bool> || std::same_as<T, std::string>
                     || std::same_as<T, std::string_view>
        std::vector<T>
        all(std::string const& key) const
        {
            std::vector<T> result {};
            for (auto const& sv : all_raw(key))
            {
                result.push_back(convert<T>(sv));
            }
            return result;
        }

        template <typename T>
            requires std::integral<T> || std::floating_point<T> || std::same_as<T, bool>
                     || std::convertible_to<T, std::string_view>
        void
        add(std::string const& key, T const& val)
        {
            if constexpr (std::same_as<T, bool>)
            {
                add_raw(key, val ? "true" : "false");
            }
            else if constexpr (std::same_as<T, std::string>)
            {
                add_raw(key, std::move(val));
            }
            else if constexpr (std::convertible_to<T, std::string_view>)
            {
                add_raw(key, std::string(std::string_view(val)));
            }
            else
            {
                add_raw(key, std::to_string(val));
            }
        }

        bool has(std::string const& key) const;
        bool empty() const;
        container_type const& params() const;

        bool decode(std::string_view content);
        std::string encoded() const;

      private:
        void add_raw(std::string const& key, std::string const& val);
        std::string_view at_raw(std::string const& key) const;
        std::vector<std::string_view> all_raw(std::string const& key) const;

        template <typename T>
            requires std::integral<T> || std::floating_point<T> || std::same_as<T, bool> || std::same_as<T, std::string>
                     || std::same_as<T, std::string_view>
        static T
        convert(std::string_view sv)
        {
            if constexpr (std::same_as<T, std::string_view>)
            {
                return sv;
            }
            else if constexpr (std::same_as<T, std::string>)
            {
                return std::string(sv);
            }
            else if constexpr (std::same_as<T, bool>)
            {
                if (sv == "1" || sv == "true")
                {
                    return true;
                }
                if (sv == "0" || sv == "false")
                {
                    return false;
                }
                throw std::runtime_error("query_param: invalid bool: " + std::string(sv));
            }
            else
            {
                return util::from_chars_strict<T>(sv);
            }
        }

        container_type params_;
    };
} // namespace httplib::html