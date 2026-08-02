
#include "httplib/html/form_data.hpp"
#include <sstream>
namespace httplib::html
{

    std::optional<form_data::field>
    form_data::field_by_name(std::string_view field_name) const
    {
        auto const& it = std::find_if(std::cbegin(fields),
                                      std::cend(fields),
                                      [&field_name](auto const& ef) { return ef.name == field_name; });

        if (it == std::cend(fields))
        {
            return {};
        }

        return *it;
    }

    bool
    form_data::has_data(std::string_view field_name) const
    {
        return field_by_name(field_name).has_value();
    }

    bool
    form_data::has_content(std::string_view field_name) const
    { // Retrieve field
        auto const& field = field_by_name(field_name);
        if (!field)
        {
            return false;
        }

        // Check if field data has content
        return field->has_data();
    }

    std::optional<std::string>
    form_data::content(std::string_view field_name) const
    { // Retrieve field
        auto const& field = field_by_name(field_name);
        if (!field)
        {
            return {};
        }

        // Check whether there is any content
        if (!field->has_data())
        {
            return {};
        }

        // Return content
        return field->content;
    }

    std::string
    form_data::dump() const
    {
        std::ostringstream ss;

        for (auto const& field : fields)
        {
            if (!field.has_data())
            {
                continue;
            }

            ss << field.name << ":\n";
            ss << "  type     = " << field.content_type << "\n";
            ss << "  filename = " << field.filename << "\n";
            if (field.file_path)
            {
                ss << "  file     = " << field.file_path->string() << "\n";
            }
            else
            {
                ss << "  content  = " << field.content << "\n";
            }
            ss << "\n";
        }

        return ss.str();
    }

} // namespace httplib::html