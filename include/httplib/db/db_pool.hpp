#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#    include "httplib/config.hpp"
#    include "httplib/db/db_config.hpp"
#    include "httplib/db/db_fwd.hpp"
#    include <memory>

namespace httplib::db
{

    class HTTPLIB_API db_pool
    {
      public:
        db_pool(net::any_io_executor ex, db_config config);
        ~db_pool();

        db_pool(db_pool const&) = delete;
        db_pool& operator=(db_pool const&) = delete;
        db_pool(db_pool&&) noexcept;
        db_pool& operator=(db_pool&&) noexcept;

        void start();
        net::awaitable<db_session> get_session();
        void cancel();

        static constexpr char const* conn_key = "httplib.db.conn";

      private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
