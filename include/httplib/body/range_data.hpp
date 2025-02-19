#pragma once

#include "httplib/config.hpp"
#include <boost/beast/core/file.hpp>
#include <utility>
#include <vector>

namespace httplib::body {

class range_data {
    using range_type = std::pair<int64_t, int64_t>;

    std::vector<range_type> ranges;
    beast::file file;
};
} // namespace httplib::body