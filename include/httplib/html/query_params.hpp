#pragma once
#include <charconv>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace httplib::html {
class query_params
{
public:
    using container_type = std::unordered_multimap<std::string, std::string>;

    std::string_view at(const std::string& key) const;

    template<typename T>
    T at_number(const std::string& key) const
    {
        auto v = at(key);

        T out {};
        auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
        if (ec != std::errc {})
            throw std::runtime_error("invalid param: " + key);

        return out;
    }

    std::vector<std::string_view> all(const std::string& key) const;

    template<typename T>
    std::vector<T> all_number(const std::string& key) const
    {
        std::vector<T> result {};
        for (const auto& v : all(key)) {
            T out {};
            auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
            if (ec != std::errc {})
                throw std::runtime_error("invalid param: " + key);

            result.push_back(out);
        }

        return result;
    }
    void add(const std::string& key, const std::string& val);
    template<typename T>
    void add_number(const std::string& key, const T& val)
    {
        add(key, std::to_string(val));
    }

    bool exists(const std::string& key) const;
    bool empty() const;
    const container_type& params() const;

    bool decode(std::string_view content);
    std::string encoded() const;

private:
    container_type params_;
};
} // namespace httplib::html