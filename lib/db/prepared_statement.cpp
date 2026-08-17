#include "httplib/db/prepared_statement.hpp"
#include "httplib/db/session.hpp"
#include "render.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace httplib::db
{
    struct prepared_statement::impl
    {
        session* session = nullptr;
        std::string original_sql;            ///< 原始 `:name` SQL（日志与渲染用）
        std::vector<detail::binder> binders; ///< 统一参数存储：位置绑定 name 为空串，命名绑定同名去重
        bool need_params_reset = false;      ///< execute 后置位，下次位置 bind 时清空位置项再重建
    };

    prepared_statement::prepared_statement(session& sess, std::string sql) : impl_(std::make_unique<impl>())
    {
        impl_->session = &sess;
        impl_->original_sql = sql;
    }

    prepared_statement::prepared_statement(prepared_statement&&) noexcept = default;
    prepared_statement& prepared_statement::operator=(prepared_statement&&) noexcept = default;
    prepared_statement::~prepared_statement() = default;

    prepared_statement&
    prepared_statement::bind_param(detail::param v)
    {
        if (impl_->need_params_reset)
        {
            impl_->binders.erase(std::remove_if(impl_->binders.begin(),
                                                impl_->binders.end(),
                                                [](detail::binder const& b) { return b.name.empty(); }),
                                 impl_->binders.end());
            impl_->need_params_reset = false;
        }
        impl_->binders.push_back({ {}, std::move(v) });
        return *this;
    }

    prepared_statement&
    prepared_statement::bind_param(std::string_view name, detail::param v)
    {
        auto it = std::find_if(impl_->binders.begin(),
                               impl_->binders.end(),
                               [&](detail::binder const& b) { return b.name == name; });
        if (it != impl_->binders.end())
        {
            it->value = std::move(v);
        }
        else
        {
            impl_->binders.push_back({ std::string(name), std::move(v) });
        }
        return *this;
    }

    net::awaitable<result>
    prepared_statement::execute()
    {

        auto res = co_await impl_->session->execute_query(impl_->original_sql, impl_->binders, true);

        // 位置绑定是消费式的：execute 后置位，下次位置 bind 时清空重建（命名绑定同名替换、可保留重绑）。
        impl_->need_params_reset = true;
        co_return res;
    }

} // namespace httplib::db