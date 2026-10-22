#include "httplib/client/response.hpp"
#include "headers_impl.hpp"
#include "httplib/client/error.hpp"
#include "ndjson_reader_impl.hpp"
#include "response_impl.h"
#include "sse_reader_impl.hpp"
#include <array>
#include <boost/beast/http/error.hpp>
#include <boost/system/system_error.hpp>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <variant>
#include "beast_alias.hpp"
#include "enum_conv.hpp"

namespace httplib::client
{

    namespace
    {
        /// \brief 空响应（默认构造或已移出）上的「需要真实响应」入口统一收口。
        [[noreturn]] void
        throw_not_initialized()
        {
            throw boost::system::system_error(make_error_code(error::not_initialized));
        }
    } // namespace

    response::response() = default;

    response::response(response&&) noexcept = default;
    response& response::operator=(response&&) noexcept = default;
    response::~response() = default;

    response::response(std::shared_ptr<impl> impl) : impl_(std::move(impl)) {}

    httplib::status
    response::result() const
    {
        return impl_ ? enum_conv::to_status(impl_->get().result()) : httplib::status::unknown;
    }

    unsigned
    response::result_int() const
    {
        return impl_ ? impl_->get().result_int() : 0u;
    }

    std::string_view
    response::operator[](httplib::field name) const
    {
        return impl_ ? std::string_view(impl_->get()[enum_conv::field_name(name)]) : std::string_view{};
    }

    std::string_view
    response::operator[](std::string_view name) const
    {
        return impl_ ? std::string_view(impl_->get()[name]) : std::string_view{};
    }

    std::string_view
    response::at(httplib::field name) const
    {
        if (!impl_)
        {
            throw std::out_of_range("response::at: empty response");
        }
        return impl_->get().at(enum_conv::field_name(name));
    }

    std::string_view
    response::at(std::string_view name) const
    {
        if (!impl_)
        {
            throw std::out_of_range("response::at: empty response");
        }
        return impl_->get().at(name);
    }

    bool
    response::has(httplib::field name) const
    {
        return impl_ && impl_->get().find(enum_conv::field_name(name)) != impl_->get().end();
    }

    bool
    response::has(std::string_view name) const
    {
        return impl_ && impl_->get().find(name) != impl_->get().end();
    }

    std::size_t
    response::count(httplib::field name) const
    {
        return impl_ ? impl_->get().count(enum_conv::field_name(name)) : 0;
    }

    std::size_t
    response::count(std::string_view name) const
    {
        return impl_ ? impl_->get().count(name) : 0;
    }

    httplib::headers
    response::headers() const
    {
        // shared_ptr 的 const 不传递到被指对象，impl_ 可变，视图直接借用消息的字段集合。
        // 空响应没有可借用的集合，返回自有空集合。
        return impl_ ? httplib::headers::impl::borrow(impl_->get()) : httplib::headers{};
    }

    httplib::headers
    response::headers()
    {
        return impl_ ? httplib::headers::impl::borrow(impl_->get()) : httplib::headers{};
    }

    httplib::headers
    response::base() const
    {
        return headers();
    }

    httplib::headers
    response::base()
    {
        return headers();
    }

    std::optional<std::uint64_t>
    response::content_length() const
    {
        if (!impl_)
        {
            return std::nullopt;
        }
        auto len = impl_->content_length();
        if (!len)
        {
            return std::nullopt;
        }
        return len.value();
    }

    std::string const&
    response::as_string() const
    {
        if (!impl_)
        {
            throw_not_initialized();
        }
        return impl_->state().as<std::string>();
    }

    boost::json::value const&
    response::as_json() const
    {
        if (!impl_)
        {
            throw_not_initialized();
        }
        return impl_->state().as<boost::json::value>();
    }

    httplib::form_data const&
    response::as_form_data() const
    {
        if (!impl_)
        {
            throw_not_initialized();
        }
        return impl_->state().as<httplib::form_data>();
    }

    httplib::query_params const&
    response::as_query_params() const
    {
        if (!impl_)
        {
            throw_not_initialized();
        }
        return impl_->state().as<httplib::query_params>();
    }

    body_type
    response::type() const
    {
        return impl_ ? impl_->state().type() : body_type::none;
    }

    std::unique_ptr<sse_reader>
    response::create_sse_reader()
    {
        if (!impl_)
        {
            throw_not_initialized();
        }
        return std::make_unique<sse_reader_impl>(impl_);
    }

    std::unique_ptr<ndjson_reader>
    response::create_ndjson_reader()
    {
        if (!impl_)
        {
            throw_not_initialized();
        }
        return std::make_unique<ndjson_reader_impl>(impl_);
    }

    net::awaitable<boost::system::result<std::string>>
    response::read_string()
    {
        if (!impl_)
        {
            co_return make_error_code(error::not_initialized);
        }
        co_return co_await impl_->read_string();
    }

    net::awaitable<boost::system::result<boost::json::value>>
    response::read_json()
    {
        if (!impl_)
        {
            co_return make_error_code(error::not_initialized);
        }
        co_return co_await impl_->read_json();
    }

    net::awaitable<boost::system::result<httplib::form_data>>
    response::read_form_data()
    {
        if (!impl_)
        {
            co_return make_error_code(error::not_initialized);
        }
        co_return co_await impl_->read_form_data();
    }

    net::awaitable<boost::system::result<httplib::query_params>>
    response::read_query_params()
    {
        if (!impl_)
        {
            co_return make_error_code(error::not_initialized);
        }
        co_return co_await impl_->read_query_params();
    }

    net::awaitable<boost::system::error_code>
    response::read_body()
    {
        if (!impl_)
        {
            co_return make_error_code(error::not_initialized);
        }
        co_return co_await impl_->read_body();
    }

    net::awaitable<boost::system::error_code>
    response::drain()
    {
        if (!impl_)
        {
            co_return make_error_code(error::not_initialized);
        }

        // 固定小块缓冲，边读边丢；不经过 sink/body_state，避免把 3xx body
        // 整体物化到内存。读线上原始字节，不做 content-encoding 解码。
        std::array<char, 8192> buf;
        for (;;)
        {
            boost::system::error_code ec;
            auto const n = co_await impl_->read_some_raw(net::buffer(buf), ec);
            if (ec)
            {
                co_return ec;
            }
            if (n == 0)
            {
                co_return boost::system::error_code {};
            }
        }
    }

    net::awaitable<boost::system::error_code>
    response::read_to_file(fs::path const& save_path)
    {
        if (!impl_)
        {
            co_return make_error_code(error::not_initialized);
        }
        co_return co_await impl_->read_to_file(save_path);
    }

    net::awaitable<std::size_t>
    response::read_some_raw(net::mutable_buffer const& buffer, boost::system::error_code& ec)
    {
        if (!impl_)
        {
            ec = make_error_code(error::not_initialized);
            co_return 0;
        }
        co_return co_await impl_->read_some_raw(buffer, ec);
    }
    net::awaitable<std::size_t>
    response::read_some_raw(net::mutable_buffer const& buffer)
    {
        boost::system::error_code ec;
        auto bytes = co_await read_some_raw(buffer, ec);
        if (ec)
        {
            throw boost::system::system_error(ec);
        }
        co_return bytes;
    }

    net::awaitable<std::size_t>
    response::read_some_decoded(net::mutable_buffer const& buffer, boost::system::error_code& ec)
    {
        if (!impl_)
        {
            ec = make_error_code(error::not_initialized);
            co_return 0;
        }
        co_return co_await impl_->read_some_decoded(buffer, ec);
    }

    net::awaitable<std::size_t>
    response::read_some_decoded(net::mutable_buffer const& buffer)
    {
        boost::system::error_code ec;
        auto bytes = co_await read_some_decoded(buffer, ec);
        if (ec)
        {
            throw boost::system::system_error(ec);
        }
        co_return bytes;
    }

    bool
    response::is_body_done() const
    {
        // 空响应没有待读内容，视为已结束。
        return !impl_ || impl_->is_body_done();
    }

} // namespace httplib::client
