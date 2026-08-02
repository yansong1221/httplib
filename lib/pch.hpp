#pragma once

// Standard library
#include <algorithm>
#include <any>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Boost
#include <boost/algorithm/string/join.hpp>
#include <boost/asio.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>

// Logging
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

// Formatting
#include <fmt/format.h>

// Project config
#include "httplib/config.hpp"
