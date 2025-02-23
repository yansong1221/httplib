#pragma once
#include "httplib/body/form_data_body.hpp"
#include "httplib/body/json_body.hpp"
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/file_body.hpp>
#include <boost/beast/http/string_body.hpp>
#include "httplib/body/any_body.hpp"
namespace httplib::body
{
using string_body = http::string_body;
using empty_body = http::empty_body;
} // namespace httplib::body