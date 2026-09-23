#include "httplib/util/misc.hpp"
#include <boost/algorithm/string/trim.hpp>

namespace httplib::util
{

    std::vector<std::string_view>
    split(std::string_view str, std::string_view delimiter, bool compress)
    {
        // Sanity check str
        if (str.empty())
        {
            return {};
        }

        // Sanity check delimiter
        if (delimiter.empty())
        {
            return { str };
        }

        // Split
        std::vector<std::string_view> parts;
        std::string_view::size_type pos = 0;
        while (pos != std::string_view::npos)
        {
            // Look for substring
            auto const pos_found = str.find(delimiter, pos);

            // Drop leading delimiters
            if (pos_found == 0)
            {
                pos += delimiter.size();
                continue;
            }

            auto s = str.substr(pos, pos_found - pos);
            if (compress)
            {
                s = boost::trim_copy(s);
            }
            // Capture string
            parts.emplace_back(s);

            // Drop trailing delimiters
            if (pos_found + delimiter.size() >= str.size())
            {
                break;
            }

            // Move on
            if (pos_found == std::string_view::npos)
            {
                break;
            }
            pos = pos_found + delimiter.size();
        }

        return parts;
    }

    std::string_view
    buffer_to_string_view(boost::asio::const_buffer const& buffer)
    {
        return std::string_view(static_cast<char const*>(buffer.data()), buffer.size());
    }

} // namespace httplib::util
