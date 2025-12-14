#pragma once
#include "httplib/config.hpp"
#include <unordered_map>
#include <utility>
#include <vector>

namespace httplib {
namespace html {
using range_type   = std::pair<int64_t, int64_t>;
using http_ranges  = std::vector<range_type>;
using query_params = std::unordered_multimap<std::string, std::string>;
} // namespace html
} // namespace httplib