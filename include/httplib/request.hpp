#pragma once
#include "variant_message.hpp"

namespace httplib {

struct request : public http_request_variant {
    using http_request_variant::http_request_variant;

public:
    http::verb method() const {
        return std::visit([](auto &t) { return t.method(); }, *this);
    }
    auto target() const {
        return std::visit([](auto &t) { return t.target(); }, *this);
    }

public:
    std::unordered_map<std::string, std::string> params;
    std::smatch matches;
    net::ip::tcp::endpoint local_endpoint;
    net::ip::tcp::endpoint remote_endpoint;
};


} // namespace httplib