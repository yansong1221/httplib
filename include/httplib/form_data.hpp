#pragma once
#include "httplib/config.hpp"
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace httplib
{
    /**
     * Type to represent the data held by an HTML form.
     *
     * Move-only: one form_data is the single holder of its fields' `file_path`,
     * so ownership of those files is never ambiguous. When
     * `params.remove_uploaded_files` is true, the destructor removes every
     * field's `file_path`.
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

            bool
            has_data() const
            {
                return !content.empty() || file_path.has_value();
            }
            bool
            is_file() const
            {
                return !filename.empty();
            }
        };

        /**
         * The parsing configuration for a form.
         * - `max_fields` (default 128) caps the number of fields the parser
         *   accepts;
         * - `max_file_size` caps the content size of a single part — file or
         *   regular field alike — whether the content is written to `save_dir`
         *   or buffered in memory; `0` means unlimited;
         * - `remove_uploaded_files` makes a form_data remove every field's
         *   `file_path` when it is destroyed (default false). It covers both the
         *   parts the parser wrote to `save_dir` and the temporary files a
         *   client attached to a request body, so either side can let the
         *   library clean up after the transfer. Leave it false when the files
         *   belong to the caller and must outlive the form_data.
         */
        struct param
        {
            fs::path save_dir;
            std::uint64_t max_file_size = 0;
            std::size_t max_fields = 128;
            bool remove_uploaded_files = false;
        };

        /**
         * The data for each field.
         */
        std::vector<field> fields;

        std::string boundary;
        param params;

        form_data() = default;
        form_data(form_data const& other) = delete;
        form_data& operator=(form_data const& other) = delete;
        form_data(form_data&& other) noexcept;
        form_data& operator=(form_data&& other) noexcept;
        ~form_data();

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

} // namespace httplib
