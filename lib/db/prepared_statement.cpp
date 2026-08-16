#include "httplib/db/prepared_statement.hpp"
#include "httplib/db/session.hpp"
#include "prepared_statement_impl.h"
#include "render.hpp"
#include <algorithm>
#include <boost/json/serialize.hpp>
#include <stdexcept>
#include <utility>

namespace httplib::db
{

    prepared_statement::prepared_statement(session& sess, std::string sql) : impl_(std::make_unique<impl>())
    {
        impl_->session = &sess;
        impl_->original_sql = sql;
        if (sql.find(':') != std::string::npos)
        {
            auto [rewritten, names] = detail::parse_placeholders(sql);
            impl_->sql = std::move(rewritten);
            impl_->param_names = std::move(names);
        }
        else
        {
            impl_->sql = std::move(sql);
        }
    }

    prepared_statement::prepared_statement(prepared_statement&&) noexcept = default;
    prepared_statement& prepared_statement::operator=(prepared_statement&&) noexcept = default;
    prepared_statement::~prepared_statement() = default;

    prepared_statement&
    prepared_statement::bind(std::string_view v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(std::string(v));
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(std::string const& v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(v);
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(char const* v)
    {
        return bind(std::string_view(v));
    }
    prepared_statement&
    prepared_statement::bind(int64_t v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(v);
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(uint64_t v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(v);
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(int v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(static_cast<int64_t>(v));
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(unsigned v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(static_cast<uint64_t>(v));
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(short v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(static_cast<int64_t>(v));
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(unsigned short v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(static_cast<uint64_t>(v));
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(double v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(v);
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(float v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(static_cast<double>(v));
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(bool v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(static_cast<int64_t>(v ? 1 : 0));
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(std::nullptr_t)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(std::monostate {});
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(date v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(v);
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(datetime v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(v);
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(time v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(v);
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(std::span<std::byte const> v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(v);
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(boost::json::value const& v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(boost::json::serialize(v));
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(std::chrono::system_clock::time_point tp)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(tp);
        return *this;
    }

    prepared_statement&
    prepared_statement::bind(std::string_view name, std::string_view v)
    {
        impl_->begin_named(name);
        impl_->named_values[std::string(name)] = std::string(v);
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(std::string_view name, char const* v)
    {
        return bind(name, std::string_view(v));
    }
    prepared_statement&
    prepared_statement::bind(std::string_view name, int64_t v)
    {
        impl_->begin_named(name);
        impl_->named_values[std::string(name)] = v;
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(std::string_view name, uint64_t v)
    {
        impl_->begin_named(name);
        impl_->named_values[std::string(name)] = v;
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(std::string_view name, int v)
    {
        return bind(name, static_cast<int64_t>(v));
    }
    prepared_statement&
    prepared_statement::bind(std::string_view name, unsigned v)
    {
        return bind(name, static_cast<uint64_t>(v));
    }
    prepared_statement&
    prepared_statement::bind(std::string_view name, short v)
    {
        return bind(name, static_cast<int64_t>(v));
    }
    prepared_statement&
    prepared_statement::bind(std::string_view name, unsigned short v)
    {
        return bind(name, static_cast<uint64_t>(v));
    }
    prepared_statement&
    prepared_statement::bind(std::string_view name, double v)
    {
        impl_->begin_named(name);
        impl_->named_values[std::string(name)] = v;
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(std::string_view name, float v)
    {
        return bind(name, static_cast<double>(v));
    }
    prepared_statement&
    prepared_statement::bind(std::string_view name, bool v)
    {
        return bind(name, static_cast<int64_t>(v ? 1 : 0));
    }
    prepared_statement&
    prepared_statement::bind(std::string_view name, std::nullptr_t)
    {
        impl_->begin_named(name);
        impl_->named_values[std::string(name)] = std::monostate {};
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(std::string_view name, date v)
    {
        impl_->begin_named(name);
        impl_->named_values[std::string(name)] = v;
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(std::string_view name, datetime v)
    {
        impl_->begin_named(name);
        impl_->named_values[std::string(name)] = v;
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(std::string_view name, time v)
    {
        impl_->begin_named(name);
        impl_->named_values[std::string(name)] = v;
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(std::string_view name, std::span<std::byte const> v)
    {
        impl_->begin_named(name);
        impl_->named_values[std::string(name)] = v;
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(std::string_view name, boost::json::value const& v)
    {
        impl_->begin_named(name);
        impl_->named_values[std::string(name)] = boost::json::serialize(v);
        return *this;
    }
    prepared_statement&
    prepared_statement::bind(std::string_view name, std::chrono::system_clock::time_point tp)
    {
        impl_->begin_named(name);
        impl_->named_values[std::string(name)] = tp;
        return *this;
    }

    prepared_statement&
    prepared_statement::bind_param(detail::param v)
    {
        impl_->begin_bind();
        impl_->params.emplace_back(std::move(v));
        return *this;
    }

    prepared_statement&
    prepared_statement::bind_param(std::string_view name, detail::param v)
    {
        impl_->begin_named(name);
        impl_->named_values[std::string(name)] = std::move(v);
        return *this;
    }

    net::awaitable<result>
    prepared_statement::execute()
    {
        // 统一走 render_query：数组参数会展开为多个 `?` 占位符。
        std::vector<detail::binder> binders;
        if (impl_->has_named_bind || (!impl_->has_positional_bind && !impl_->param_names.empty()))
        {
            binders.reserve(impl_->param_names.size());
            for (auto const& name : impl_->param_names)
            {
                auto it = impl_->named_values.find(name);
                if (it == impl_->named_values.end())
                {
                    throw std::runtime_error("db: unbound named parameter '" + name + "'");
                }
                binders.push_back({ name, it->second });
            }
        }
        else
        {
            binders.reserve(impl_->params.size());
            for (auto const& p : impl_->params)
            {
                binders.push_back({ {}, p });
            }
        }

        auto rendered = detail::render_query(impl_->original_sql, binders);

        std::vector<std::string> names;
        if (!impl_->has_positional_bind)
        {
            names = impl_->param_names;
        }

        impl_->need_params_reset = true;
        co_return co_await impl_->session->execute_prepared(rendered.sql,
                                                            std::move(rendered.params),
                                                            impl_->original_sql,
                                                            std::move(names),
                                                            !rendered.expanded);
    }

} // namespace httplib::db
