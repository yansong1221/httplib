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

    net::awaitable<result>
    prepared_statement::execute()
    {
        // 命名绑定（或未绑定的 :name 语句）→ 从 named_values 重建参数；纯位置绑定 → 直接用 params
        if (impl_->has_named_bind || (!impl_->has_positional_bind && !impl_->param_names.empty()))
        {
            impl_->params.clear();
            impl_->params.reserve(impl_->param_names.size());
            for (auto const& name : impl_->param_names)
            {
                auto it = impl_->named_values.find(name);
                if (it == impl_->named_values.end())
                {
                    throw std::runtime_error("db: unbound named parameter '" + name + "'");
                }
                impl_->params.push_back(it->second);
            }
        }

        std::vector<detail::param> params;
        params.reserve(impl_->params.size());
        for (auto const& p : impl_->params)
        {
            params.push_back(std::visit([](auto const& v) -> detail::param { return v; }, p));
        }

        std::vector<std::string> names;
        if (!impl_->has_positional_bind)
        {
            names = impl_->param_names;
        }

        impl_->need_params_reset = true;
        co_return co_await impl_->session->execute_prepared(impl_->sql,
                                                            std::move(params),
                                                            impl_->original_sql,
                                                            std::move(names));
    }

} // namespace httplib::db
