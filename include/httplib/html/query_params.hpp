#pragma once
#include <string>
#include <unordered_map>

namespace httplib::html {
class query_params
{
public:
    std::string_view at(const std::string& key) const;
    std::vector<std::string_view> all(const std::string& key) const;
    bool exists(const std::string& key) const;
    bool empty() const;

    bool decode(std::string_view content);
    std::string encoded() const;

private:
    std::unordered_multimap<std::string, std::string> params_;
};
} // namespace httplib::html