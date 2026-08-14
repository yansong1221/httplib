#pragma once
#ifdef HTTPLIB_ENABLED_DATABASE

#include <boost/system/error_code.hpp>
#include <stdexcept>

namespace httplib::mysql
{

    /**
     * \brief MySQL 模块抛出的异常类型。
     * \details
     * 携带一个 boost::system::error_code，用于区分服务端 SQL 错误与客户端/传输错误。
     */
    class mysql_exception : public std::runtime_error
    {
      public:
        /**
         * \brief 构造异常。
         * \param ec 错误码。
         * \param what 人类可读的错误信息。
         */
        mysql_exception(boost::system::error_code ec, std::string const& what) : std::runtime_error(what), ec_(ec) {}

        /**
         * \brief 返回错误码。
         */
        boost::system::error_code const&
        code() const noexcept
        {
            return ec_;
        }

      private:
        boost::system::error_code ec_;
    };

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
