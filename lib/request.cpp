#include "httplib/request.hpp"

namespace httplib
{

request::request(http::request<body::any_body>&& other)
{
    http::request<body::any_body>::operator=(std::move(other));
}

} // namespace httplib