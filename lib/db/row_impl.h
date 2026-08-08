#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#    include "httplib/db/row.hpp"

namespace httplib::db
{

    struct row::impl
    {
        db_result const* parent = nullptr;
        size_t idx = 0;

        size_t col_of(std::string_view name) const;
    };

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
