#include "httplib/client/error.hpp"
#include <string>

namespace httplib::client
{
    namespace
    {
        struct error_category : boost::system::error_category
        {
            char const*
            name() const noexcept override
            {
                return "httplib.client";
            }
            std::string
            message(int ev) const override
            {
                switch (static_cast<error>(ev))
                {
                    case error::http_status:
                        return "server returned a non-success status";
                    case error::too_many_redirects:
                        return "too many redirects";
                    case error::protocol_violation:
                        return "protocol violation";
                }
                return "unknown error";
            }
        };

        boost::system::error_category const&
        client_category() noexcept
        {
            static error_category cat;
            return cat;
        }
    } // namespace

    boost::system::error_code
    make_error_code(error e)
    {
        return { static_cast<int>(e), client_category() };
    }
} // namespace httplib::client
