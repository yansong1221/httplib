#include "httplib/error.hpp"
#include <string>

namespace httplib
{
    namespace
    {
        struct error_category : boost::system::error_category
        {
            char const*
            name() const noexcept override
            {
                return "httplib";
            }

            std::string
            message(int ev) const override
            {
                switch (static_cast<error>(ev))
                {
                    case error::not_found:
                        return "not found";
                    case error::invalid_value:
                        return "value cannot be converted to the requested type";
                }
                return "unknown error";
            }
        };

        boost::system::error_category const&
        httplib_category() noexcept
        {
            static error_category cat;
            return cat;
        }
    } // namespace

    boost::system::error_code
    make_error_code(error e)
    {
        return { static_cast<int>(e), httplib_category() };
    }
} // namespace httplib
