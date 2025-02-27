#pragma once
#include "variant_stream.hpp"

namespace httplib
{

template<typename... T>
class http_variant_stream : public variant_stream<T...>
{
public:
    using variant_stream<T...>::variant_stream;

public:
    auto expires_after(const net::steady_timer::duration& expiry_time)
    {
        return std::visit([&](auto& t) mutable { return beast::get_lowest_layer(t).expires_after(expiry_time); },
                          *this);
    }
    auto expires_never()
    {
        return std::visit([&](auto& t) mutable { return beast::get_lowest_layer(t).expires_never(); }, *this);
    }

    auto& rate_policy() & noexcept
    {
        return std::visit([&](auto& t) mutable -> auto& { return beast::get_lowest_layer(t).rate_policy(); }, *this);
    }
    auto& rate_policy() const& noexcept
    {
        return std::visit([&](auto& t) -> auto& { return beast::get_lowest_layer(t).rate_policy(); }, *this);
    }
};

} // namespace httplib