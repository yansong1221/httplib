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
                    case error::invalid_url:
                        return "invalid URL";
                    case error::truncated_body:
                        return "response body is shorter than declared";
                    case error::cache_unavailable:
                        return "cached body is unavailable for revalidation";
                    case error::acquire_timeout:
                        return "timed out acquiring a connection from the pool";
                    case error::invalid_handle:
                        return "handle does not own a connection";
                    case error::not_initialized:
                        return "object is not bound to a stream";
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
