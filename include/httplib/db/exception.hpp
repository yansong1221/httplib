#pragma once
#include <boost/system/error_code.hpp>
#include <stdexcept>
#include <string>

namespace httplib::db
{

    /**
     * \brief 数据库模块抛出的异常类型。
     * \details
     * 携带一个 boost::system::error_code，用于区分服务端错误与客户端/传输错误。
     */
    class db_exception : public std::runtime_error
    {
      public:
        db_exception(boost::system::error_code ec, std::string const& what) : std::runtime_error(what), ec_(ec) {}

        /// 返回错误码。
        boost::system::error_code const&
        code() const noexcept
        {
            return ec_;
        }

      private:
        boost::system::error_code ec_;
    };

} // namespace httplib::db
