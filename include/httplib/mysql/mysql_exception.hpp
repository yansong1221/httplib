#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include <boost/system/error_code.hpp>
#include <stdexcept>

namespace httplib::mysql
{

    class mysql_exception : public std::runtime_error
    {
      public:
        mysql_exception(boost::system::error_code ec, std::string const& what) : std::runtime_error(what), ec_(ec) {}

        boost::system::error_code const&
        code() const noexcept
        {
            return ec_;
        }

      private:
        boost::system::error_code ec_;
    };

} // namespace httplib::mysql
#endif
