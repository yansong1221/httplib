#pragma once

#include <string>

namespace httplib::body {
/**
     * Type to represent the data held by a single field of an HTML form.
     */
struct form_field_data {
    std::string name; /// The field name.
    std::string filename;
    std::string content_type;
    std::string content;

    [[nodiscard]] bool has_data() const {
        return !content.empty();
    }

    bool is_file() const {
        return !filename.empty();
    }
};

} // namespace httplib
