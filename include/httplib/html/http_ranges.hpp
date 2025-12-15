#pragma once
#include <string_view>
#include <vector>

namespace httplib::html {
class http_ranges
{
public:
    using range_type  = std::pair<int64_t, int64_t>;
    using ranges_type = std::vector<range_type>;

public:
    std::size_t size() const;
    bool empty() const;

    const range_type& front() const;
    const range_type& at(std::size_t index) const;

    void append(const range_type& range);

    const ranges_type& ranges() const;
    bool parse(std::string_view range_str, size_t file_size);

private:
    ranges_type ranges_;
};
} // namespace httplib::html