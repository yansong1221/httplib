#pragma once
#include <memory>
#include <spdlog/spdlog.h>

namespace httplib {
class connection : public std::enable_shared_from_this<connection> {
private:
};
} // namespace httplib