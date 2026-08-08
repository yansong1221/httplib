#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/db/transaction.hpp"
#include "db/db_session_impl.h"
#include "httplib/db/db_session.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql.hpp>

namespace httplib::db
{

    transaction::transaction(db_session& sess) : impl_(std::make_unique<impl>()) { impl_->session = &sess; }

    transaction::transaction(transaction&&) noexcept = default;
    transaction& transaction::operator=(transaction&&) noexcept = default;

    transaction::~transaction()
    {
        if (impl_ && impl_->session && !impl_->committed)
        {
            auto& si = get_impl(*impl_->session);
            if (si.in_transaction)
            {
                auto ex = si.pooled.get().get_executor();
                net::co_spawn(
                    ex,
                    [&si]() -> net::awaitable<void>
                    {
                        try
                        {
                            co_await si.rollback();
                        }
                        catch (...)
                        {
                        }
                    },
                    net::detached);
            }
        }
    }

    net::awaitable<void>
    transaction::commit()
    {
        co_await get_impl(*impl_->session).commit();
        impl_->committed = true;
    }

    net::awaitable<transaction>
    db_session::begin()
    {
        co_await get_impl(*this).begin_transaction();
        co_return transaction(*this);
    }

} // namespace httplib::db
#endif // HTTPLIB_ENABLED_DATABASE
