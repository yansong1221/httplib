#include "httplib/setting.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace httplib {

server::setting::setting()
{
    auto console_sink                 = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    spdlog::sinks_init_list sink_list = {console_sink};
    default_logger_ = std::make_shared<spdlog::logger>("httplib.server", sink_list);
    default_logger_->set_level(spdlog::level::info);
}
std::shared_ptr<spdlog::logger> server::setting::get_logger() const
{
    if (custom_logger_)
        return custom_logger_;
    return default_logger_;
}

void server::setting::set_logger(std::shared_ptr<spdlog::logger> logger)
{
    custom_logger_ = logger;
}

} // namespace httplib