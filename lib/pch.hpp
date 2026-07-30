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

// Boost.Asio
#include <boost/asio.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>

// Boost.Beast
#include <boost/beast.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/version.hpp>

// Boost.JSON
#include <boost/json.hpp>

// Boost.Algorithm
#include <boost/algorithm/string/join.hpp>

// Boost.System
#include <boost/system.hpp>
#include <boost/system/result.hpp>

// Logging
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

// Formatting
#include <fmt/format.h>

// Project config (namespace aliases)
#include "httplib/config.hpp"
