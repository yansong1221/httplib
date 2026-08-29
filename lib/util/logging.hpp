#pragma once
#include <memory>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

namespace httplib::detail
{
    /**
     * \brief 进程内唯一的 stdout 彩色 sink。
     * \details 所有默认 logger 共享同一个 sink 实例，靠这一把互斥锁串行化
     * 对 stdout 的写入，避免多个 logger 各自建 sink 导致控制台输出交叉。
     */
    inline std::shared_ptr<spdlog::sinks::sink>
    console_sink()
    {
        static auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        return sink;
    }

    /**
     * \brief 用共享 sink 创建一个 info 级别的默认控制台 logger。
     */
    inline std::shared_ptr<spdlog::logger>
    make_console_logger(std::string_view name)
    {
        auto logger = std::make_shared<spdlog::logger>(std::string(name), console_sink());
        logger->set_level(spdlog::level::info);
        return logger;
    }

} // namespace httplib::detail
