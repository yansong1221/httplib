#pragma once
#include "httplib/config.hpp"
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace httplib::html {
/**
 * Type to represent the data held by an HTML form.
 *
 * @sa form
 */
class HTTPLIB_API form_data
{
public:
    struct field
    {
        std::string name; /// The field name.
        std::string filename;
        std::string content_type;
        std::string content;
        std::optional<fs::path> file_path;

        bool has_data() const { return !content.empty() || file_path.has_value(); }
        bool is_file() const { return !filename.empty(); }
    };

    /**
     * The data for each field.
     */
    std::vector<field> fields;

    std::string boundary;
    fs::path save_dir;
    std::uint64_t max_file_size = 0;

    /**
     * Get a field by name.
     *
     * @param field_name The field name.
     * @return The field (if any).
     */
    std::optional<field> field_by_name(std::string_view field_name) const;
    /**
     * Checks whether a field has parsed data.
     *
     * @param field_name The name of the field.
     * @return Whether the field has parsed data.
     */
    bool has_data(std::string_view field_name) const;

    /**
     * Checks whether a particular field has parsed content.
     *
     * @param field_name The field name.
     * @return Whether the field has parsed content.
     */
    bool has_content(std::string_view field_name) const;

    /**
     * The the parsed data content of a specific field.
     *
     * @param field_name The name of the field.
     * @return
     */
    std::optional<std::string> content(std::string_view field_name) const;

    /**
     * Dumps the key-value pairs as a readable string.
     *
     * @return Key-value pairs represented as a string
     */
    std::string dump() const;
};

} // namespace httplib::html
