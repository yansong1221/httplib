#pragma once
#include "httplib/config.hpp"
#include "httplib/server/helper.hpp"
#include <boost/asio/awaitable.hpp>
#include <filesystem>

namespace httplib::server {
class request;
class response;

class mount_point_entry
{
public:
    mount_point_entry(const std::string& _mount_point, const fs::path& _base_dir);
    virtual ~mount_point_entry() = default;

public:
    const std::string& mount_point() const;
    const fs::path& base_dir() const;

public:
    net::awaitable<bool> invoke(request& req, response& res) const;

protected:
    virtual net::awaitable<bool> before(request& req, response& res) const;
    virtual net::awaitable<bool> after(request& req, response& res) const;

private:
    std::string mount_point_;
    fs::path base_dir_;
    std::vector<std::string> default_doc_name_ = {"index.html", "index.htm"};
};

template<typename... Aspects>
class aspects_mount_point_entry : public mount_point_entry
{
public:
    aspects_mount_point_entry(const std::string& _mount_point,
                              const fs::path& _base_dir,
                              Aspects&&... _asps)
        : mount_point_entry(_mount_point, _base_dir)
        , aspects_(std::forward<Aspects>(_asps)...)
    {
    }

protected:
    net::awaitable<bool> before(request& req, response& resp) const override
    {
        bool ok = true;

        co_await std::apply(
            [&](auto&... aspect) -> net::awaitable<void> {
                ((co_await helper::do_before(aspect, req, resp, ok)), ...);
            },
            aspects_);

        co_return ok;
    }
    net::awaitable<bool> after(request& req, response& resp) const override
    {
        bool ok = true;
        co_await std::apply(
            [&](auto&... aspect) -> net::awaitable<void> {
                ((co_await helper::do_after(aspect, req, resp, ok)), ...);
            },
            aspects_);

        co_return ok;
    }

private:
    std::tuple<Aspects...> aspects_;
};

} // namespace httplib::server