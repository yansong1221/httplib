#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include "httplib/mysql/config.hpp"
#include "httplib/mysql/prepared_statement.hpp"
#include "httplib/mysql/result.hpp"
#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

namespace httplib::mysql
{

    struct query_log_entry
    {
        std::string sql;
        std::chrono::steady_clock::duration duration { 0 };
        size_t row_count = 0;
        uint64_t affected_rows = 0;
        bool is_parameterized = false;
    };

    class HTTPLIB_API session
    {
      public:
        using query_logger = std::function<void(query_log_entry const&)>;

        session(session&&) noexcept;
        session& operator=(session&&) noexcept;
        ~session();

        static net::awaitable<session> connect(net::any_io_executor ex, connect_params cfg);

        net::awaitable<result> query(std::string_view sql);

        prepared_statement stmt(std::string_view sql);

        net::awaitable<void> begin_transaction();
        net::awaitable<void> commit();
        net::awaitable<void> rollback();

        template <typename F>
            requires std::invocable<F, session&>
                     && std::same_as<std::invoke_result_t<F, session&>, net::awaitable<void>>
        net::awaitable<void>
        with_transaction(F&& f)
        {
            co_await begin_transaction();
            std::exception_ptr e;
            try
            {
                co_await std::invoke(std::forward<F>(f), *this);
            }
            catch (...)
            {
                e = std::current_exception();
            }
            if (e)
            {
                try
                {
                    co_await rollback();
                }
                catch (...)
                {
                    // 回滚失败（通常是连接已死）时不掩盖原始异常
                }
                std::rethrow_exception(e);
            }
            co_await commit();
        }

        net::awaitable<bool> ping();
        net::awaitable<void> reconnect();

        void set_query_logger(query_logger cb);

        void touch();
        std::chrono::steady_clock::time_point last_active_time() const;
        std::chrono::steady_clock::time_point last_ping_time() const;
        bool in_transaction() const;

        struct impl;
        explicit session(std::unique_ptr<impl> p);

      private:
        std::unique_ptr<impl> impl_;

        friend impl& get_impl(session& self);
        friend impl const& get_impl(session const& self);
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
