#include "httplib/html/accept_content.hpp"
#include "body/compressor.hpp"
#include "httplib/util/misc.hpp"

namespace httplib::html {

bool basic_accept_content ::parse(std::string_view header_value)
{
    elements_.clear();

    auto tokens = util::split(header_value, ",");
    for (std::string_view token : tokens) {
        token = boost::trim_copy(token);
        if (!token.empty()) {
            elements_.push_back(parse_element(token));
        }
    }
    return true;
}

basic_accept_content::header_element
basic_accept_content ::parse_element(std::string_view element_str)
{
    header_element elem;
    auto parts = util::split(element_str, ";");
    if (!parts.empty()) {
        elem.value = boost::trim_copy(parts[0]);
        for (size_t i = 1; i < parts.size(); ++i) {
            std::string_view param = parts[i];

            param = boost::trim_copy(param);
            if (!param.empty()) {
                auto eqPos = param.find('=');
                if (eqPos != std::string::npos) {
                    std::string_view key   = boost::trim_copy(param.substr(0, eqPos));
                    std::string_view value = boost::trim_copy(param.substr(eqPos + 1));
                    if (!value.empty() && value.front() == '"' && value.back() == '"') {
                        value = value.substr(1, value.size() - 2);
                    }
                    elem.params[std::string(key)] = value;
                }
            }
        }
    }
    return elem;
}


std::string accept_encoding_content::server_apply_encoding() const
{
    for (const auto& elem : elements_) {
        if (httplib::body::compressor_factory::instance().is_supported_encoding(elem.value)) {
            return elem.value;
        }
    }
    return {};
}

} // namespace httplib::html