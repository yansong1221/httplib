// 公共头不能包含 Boost，所以 string_to_method() 的实现放在这里。

#include "httplib/enums.hpp"
#include "httplib/config.hpp"

#include <boost/beast/http/verb.hpp>

#include "beast_alias.hpp"
#include "enum_conv.hpp"

namespace httplib
{

    method
    string_to_method(std::string_view s) noexcept
    {
        return enum_conv::to_method(http::string_to_verb(s));
    }

} // namespace httplib
