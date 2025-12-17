#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace httplib::html {
class query_params
{
public:
    using container_type = std::unordered_multimap<std::string, std::string>;

    std::string_view at(const std::string& key) const;
    std::vector<std::string_view> all(const std::string& key) const;
    bool exists(const std::string& key) const;
    bool empty() const;
    const container_type& params() const;

    bool decode(std::string_view content);
    std::string encoded() const;

private:
    container_type params_;
};
} // namespace httplib::html