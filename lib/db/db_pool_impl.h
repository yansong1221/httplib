#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/db/db_config.hpp"
#include <boost/mysql/connection_pool.hpp>
#include <memory>

namespace httplib::db
{

    struct db_pool::impl
    {
        db_config config;
        std::shared_ptr<boost::mysql::connection_pool> pool;
    };

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
