#pragma once
#include "httplib/form_data.hpp"

#include <sstream>
namespace httplib {

std::optional<httplib::form_data::field> form_data::field_by_name(std::string_view field_name) const
{
    const auto& it = std::find_if(std::cbegin(fields),
                                  std::cend(fields),
                                  [&field_name](const auto& ef) { return ef.name == field_name; });

    if (it == std::cend(fields))
        return {};

    return *it;
}

bool form_data::has_data(std::string_view field_name) const
{
    return field_by_name(field_name).has_value();
}

bool form_data::has_content(std::string_view field_name) const
{ // Retrieve field
    const auto& field = field_by_name(field_name);
    if (!field)
        return false;

    // Check if field data has content
    return field->has_data();
}

std::optional<std::string> form_data::content(std::string_view field_name) const
{ // Retrieve field
    const auto& field = field_by_name(field_name);
    if (!field)
        return {};

    // Check whether there is any content
    if (!field->has_data())
        return {};

    // Return content
    return field->content;
}

std::string form_data::dump() const
{
    std::ostringstream ss;

    for (const auto& field : fields) {
        if (!field.has_data())
            continue;

        ss << field.name << ":\n";
        ss << "  type     = " << field.content_type << "\n";
        ss << "  filename = " << field.filename << "\n";
        ss << "  content  = " << field.content << "\n";
        ss << "\n";
    }

    return ss.str();
}

} // namespace httplib