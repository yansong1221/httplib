#include "httplib/db/error.hpp"
#include <string>

namespace httplib::db
{
    namespace
    {
        struct error_category : boost::system::error_category
        {
            char const*
            name() const noexcept override
            {
                return "httplib.db";
            }
            std::string
            message(int ev) const override
            {
                switch (static_cast<error>(ev))
                {
                    case error::invalid_parameters:
                        return "SQL parameters do not match the statement placeholders";
                    case error::column_not_found:
                        return "no such column in the result set";
                    case error::type_mismatch:
                        return "value cannot be converted to the requested type";
                    case error::null_value:
                        return "unexpected NULL value";
                    case error::not_initialized:
                        return "session is not connected or the statement is not prepared";
                    case error::acquire_timeout:
                        return "timed out acquiring a connection from the pool";
                    case error::unknown_backend:
                        return "unknown database backend";
                }
                return "unknown error";
            }
        };

        boost::system::error_category const&
        db_category() noexcept
        {
            static error_category cat;
            return cat;
        }
    } // namespace

    boost::system::error_code
    make_error_code(error e)
    {
        return { static_cast<int>(e), db_category() };
    }
} // namespace httplib::db
