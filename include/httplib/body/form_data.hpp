#pragma once
#include "form_field.hpp"
#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace httplib {

/**
     * Type to represent the data held by an HTML form.
     *
     * @sa form
     */
class form_data {
public:
    /**
         * The data for each field.
         */
    std::vector<form_field_data> fields;

    /**
         * Get a field by name.
         *
         * @param field_name The field name.
         * @return The field (if any).
         */
    [[nodiscard]] std::optional<form_field_data> field_by_name(std::string_view field_name) const {
        const auto &it =
            std::find_if(std::cbegin(fields), std::cend(fields),
                         [&field_name](const auto &ef) { return ef.name == field_name; });

        if (it == std::cend(fields))
            return {};

        return *it;
    }

    /**
         * Checks whether a field has parsed data.
         *
         * @param field_name The name of the field.
         * @return Whether the field has parsed data.
         */
    [[nodiscard]] bool has_data(std::string_view field_name) const {
        return field_by_name(field_name).has_value();
    }

    /**
         * Checks whether a particular field has parsed content.
         *
         * @param field_name The field name.
         * @return Whether the field has parsed content.
         */
    [[nodiscard]] bool has_content(std::string_view field_name) const {
        // Retrieve field
        const auto &field = field_by_name(field_name);
        if (!field)
            return false;

        // Check if field data has content
        return field->has_data();
    }

    /**
         * The the parsed data content of a specific field.
         *
         * @param field_name The name of the field.
         * @return
         */
    [[nodiscard]] std::optional<std::string> content(std::string_view field_name) const {
        // Retrieve field
        const auto &field = field_by_name(field_name);
        if (!field)
            return {};

        // Check whether there is any content
        if (!field->has_data())
            return {};

        // Return content
        return field->content;
    }

    /**
         * Dumps the key-value pairs as a readable string.
         *
         * @return Key-value pairs represented as a string
         */
    [[nodiscard]] std::string dump() const {
        std::ostringstream ss;

        for (const form_field_data &field : fields) {
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
};

} // namespace httplib
