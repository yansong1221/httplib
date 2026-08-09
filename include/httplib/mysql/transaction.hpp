#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/config.hpp"
#include "httplib/mysql/mysql_fwd.hpp"
#include <memory>

namespace httplib::mysql
{

    class HTTPLIB_API transaction
    {
      public:
        transaction(transaction&&) noexcept;
        transaction& operator=(transaction&&) noexcept;
        ~transaction();

        transaction(transaction const&) = delete;
        transaction& operator=(transaction const&) = delete;

        net::awaitable<void> commit();

        struct impl;
        explicit transaction(session& sess);

      private:
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
