#pragma once
#include <map>
#include <string>
#include <vector>

namespace httplib::html {
class basic_accept_content
{
public:
    virtual ~basic_accept_content() = default;

    struct header_element
    {
        std::string value;
        std::map<std::string, std::string> params;
    };

public:
    bool parse(std::string_view header_value);

private:
    static header_element parse_element(std::string_view element_str);

protected:
    std::vector<header_element> elements_;
};


class accept_encoding_content : public basic_accept_content
{
public:
    std::string server_apply_encoding() const;
};

} // namespace httplib::html