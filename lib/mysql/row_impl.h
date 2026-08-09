#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include "httplib/mysql/row.hpp"

namespace httplib::mysql
{

    struct row::impl
    {
        result const* parent = nullptr;
        size_t idx = 0;

        size_t col_of(std::string_view name) const;
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
