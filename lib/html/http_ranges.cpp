#include "httplib/html/http_ranges.hpp"
#include "httplib/util/misc.hpp"
#include <boost/algorithm/string/trim.hpp>
#include <charconv>

namespace httplib::html {
bool http_ranges::parse(std::string_view range_str, size_t file_size)
{
    ranges_.clear();

    range_str = boost::trim_copy(range_str);
    if (range_str.empty())
        return true;
    if (!range_str.starts_with("bytes=")) {
        return false;
    }
    range_str.remove_prefix(6);

    if (range_str.find("--") != std::string_view::npos) {
        return false;
    }

    if (range_str == "-") {
        ranges_.push_back(range_type {0, file_size - 1});
        return true;
    }
    auto ranges = util::split(range_str, ",");
    for (const auto& range : ranges) {
        auto sub_range  = util::split(range, "-");
        auto fist_range = boost::trim_copy(sub_range[0]);

        int start = 0;
        if (fist_range.empty()) {
            start = -1;
        }
        else {
            auto [ptr, ec] =
                std::from_chars(fist_range.data(), fist_range.data() + fist_range.size(), start);
            if (ec != std::errc {}) {
                ranges_.clear();
                return false;
            }
        }

        int end = 0;
        if (sub_range.size() == 1) {
            end = file_size - 1;
        }
        else {
            auto second_range = boost::trim_copy(sub_range[1]);
            if (second_range.empty()) {
                end = file_size - 1;
            }
            else {
                auto [ptr, ec] = std::from_chars(
                    second_range.data(), second_range.data() + second_range.size(), end);
                if (ec != std::errc {}) {
                    ranges_.clear();
                    return false;
                }
            }
        }

        if (start > 0 && (start >= file_size || start == end)) {
            // out of range
            ranges_.clear();
            return false;
        }

        if (end > 0 && end >= file_size) {
            end = file_size - 1;
        }

        if (start == -1) {
            start = file_size - end;
            end   = file_size - 1;
        }
        ranges_.push_back(range_type {start, end});
    }
    return true;
}

const http_ranges::ranges_type& http_ranges::ranges() const
{
    return ranges_;
}

std::size_t http_ranges::size() const
{
    return ranges_.size();
}

bool http_ranges::empty() const
{
    return ranges_.empty();
}

const http_ranges::range_type& http_ranges::at(std::size_t index) const
{
    return ranges_.at(index);
}

const http_ranges::range_type& http_ranges::front() const
{
    return ranges_.front();
}

void http_ranges::append(const range_type& range)
{
    ranges_.push_back(range);
}

} // namespace httplib::html
