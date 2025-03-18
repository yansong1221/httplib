#include "session.hpp"

namespace httplib {

session::session(tcp::socket&& sock) : stream_(std::move(sock)) { }

void session::close() { }

} // namespace httplib