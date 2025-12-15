#include "httplib/html/query_params.hpp"
#include "httplib/util/misc.hpp"
#include <boost/algorithm/string/join.hpp>
#include <stdexcept>

namespace httplib::html {
std::string_view query_params::at(const std::string& key) const
{
    auto iter = params_.find(key);
    if (iter == params_.end()) {
        throw std::runtime_error("Key not found: " + key);
    }
    return iter->second;
}

std::vector<std::string_view> query_params::all(const std::string& key) const
{
    std::vector<std::string_view> values;
    auto range = params_.equal_range(key);
    for (auto it = range.first; it != range.second; ++it) {
        values.push_back(it->second);
    }
    return values;
}

bool query_params::exists(const std::string& key) const
{
    return params_.find(key) != params_.end();
}

bool query_params::decode(std::string_view content)
{
    params_.clear();
    if (content.empty())
        return true;

    for (const auto& item : util::split(content, "&")) {
        auto key_val = util::split(item, "=");

        if (key_val.size() != 2) {
            params_.clear();
            return false;
        }
        auto key = util::url_decode(key_val[0]);
        auto val = util::url_decode(key_val[1]);

        params_.emplace(key, val);
    }
    return true;
}

std::string query_params::encoded() const
{
    std::vector<std::string> tokens;
    for (const auto& item : params_) {
        auto token = util::url_encode(item.first);
        token += "=";
        token += util::url_encode(item.second);

        tokens.push_back(token);
    }
    return boost::join(tokens, "&");
}

bool query_params::empty() const
{
    return params_.empty();
}

} // namespace httplib::html
