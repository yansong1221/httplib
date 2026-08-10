#ifdef HTTPLIB_ENABLED_DATABASE
#include "httplib/mysql/transaction.hpp"
#include "httplib/mysql/session.hpp"
#include "mysql/session_impl.h"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/mysql.hpp>

namespace httplib::mysql
{

    transaction::transaction(session& sess) : impl_(std::make_unique<impl>()) { impl_->session = &sess; }

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
                    [sess = impl_->session]() -> net::awaitable<void>
                    {
                        try
                        {
                            co_await get_impl(*sess).rollback();
                        }
                        catch (...)
                        {
                        }
                    },
                    [](std::exception_ptr e)
                    {
                        if (e)
                        {
                            std::rethrow_exception(e);
                        }
                    });
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
    session::begin()
    {
        co_await get_impl(*this).begin_transaction();
        co_return transaction(*this);
    }

} // namespace httplib::mysql
#endif // HTTPLIB_ENABLED_DATABASE
