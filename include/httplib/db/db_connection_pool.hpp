#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#    include "httplib/config.hpp"
#    include "httplib/db/db_config.hpp"
#    include "httplib/db/db_connection.hpp"
#    include <atomic>
#    include <boost/asio/any_io_executor.hpp>
#    include <boost/asio/io_context.hpp>
#    include <boost/asio/steady_timer.hpp>
#    include <deque>
#    include <functional>
#    include <memory>
#    include <mutex>
#    include <vector>

namespace httplib::db
{

    using db_connection_factory
        = std::function<net::awaitable<std::shared_ptr<db_connection>>(net::any_io_executor, db_config const&)>;

    class HTTPLIB_API db_connection_pool : public std::enable_shared_from_this<db_connection_pool>
    {
      public:
        explicit db_connection_pool(net::any_io_executor ex, db_config config, db_connection_factory factory);
        explicit db_connection_pool(boost::asio::io_context& io_ctx, db_config config, db_connection_factory factory);

        ~db_connection_pool();

        net::awaitable<void> init();
        [[nodiscard]] net::awaitable<std::shared_ptr<db_connection>> acquire();
        void release(std::shared_ptr<db_connection> conn);
        net::awaitable<void> shutdown();

        [[nodiscard]] size_t active_count() const;
        [[nodiscard]] size_t idle_count() const;
        [[nodiscard]] size_t waiting_count() const;
        [[nodiscard]] size_t total_count() const;

        static constexpr char const* pool_key = "httplib.db.pool";
        static constexpr char const* conn_key = "httplib.db.conn";

      private:
        void wake_one_waiter(std::shared_ptr<db_connection> conn);
        void start_idle_checker();
        net::awaitable<void> idle_check_loop();
        void start_health_checker();
        net::awaitable<void> health_check_loop();

        net::any_io_executor ex_;
        db_config config_;
        db_connection_factory factory_;

        mutable std::mutex mutex_;
        std::vector<std::shared_ptr<db_connection>> idle_;
        size_t active_count_ = 0;

        struct waiter
        {
            std::shared_ptr<boost::asio::steady_timer> timer;
            std::shared_ptr<std::shared_ptr<db_connection>> result;
        };

        std::deque<waiter> waiters_;
        std::atomic<bool> shutdown_ { false };
        bool initialized_ = false;

        std::shared_ptr<boost::asio::steady_timer> idle_check_timer_;
        std::shared_ptr<boost::asio::steady_timer> health_check_timer_;
    };

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
