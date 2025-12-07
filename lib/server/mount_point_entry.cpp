#include "httplib/server/mount_point_entry.hpp"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"

namespace httplib::server {

namespace detail {
inline static bool is_valid_path(std::string_view path)
{
    size_t level = 0;
    size_t i     = 0;

    // Skip slash
    while (i < path.size() && path[i] == '/') {
        i++;
    }

    while (i < path.size()) {
        // Read component
        auto beg = i;
        while (i < path.size() && path[i] != '/') {
            if (path[i] == '\0') {
                return false;
            }
            else if (path[i] == '\\') {
                return false;
            }
            i++;
        }

        auto len = i - beg;
        assert(len > 0);

        if (!path.compare(beg, len, ".")) {
            ;
        }
        else if (!path.compare(beg, len, "..")) {
            if (level == 0) {
                return false;
            }
            level--;
        }
        else {
            level++;
        }

        // Skip slash
        while (i < path.size() && path[i] == '/') {
            i++;
        }
    }

    return true;
}

} // namespace detail

mount_point_entry::mount_point_entry(const std::string& mount_point, const fs::path& base_dir)
    : mount_point_(mount_point)
    , base_dir_(base_dir)
{
}
const std::string& mount_point_entry::mount_point() const
{
    return mount_point_;
}
const httplib::fs::path& mount_point_entry::base_dir() const
{
    return base_dir_;
}
net::awaitable<bool> mount_point_entry::invoke(request& req, response& res) const
{
    std::string_view target(req.decoded_path());
    // Prefix match
    if (!target.starts_with(mount_point_))
        co_return false;

    target.remove_prefix(mount_point_.size());
    if (target.starts_with("/"))
        target.remove_prefix(1);

    if (!detail::is_valid_path(target))
        co_return false;

    std::error_code ec;
    auto path =
        base_dir_ / fs::path(std::u8string_view((const char8_t*)target.data(), target.size()));
    if (!fs::exists(path, ec))
        co_return false;

    if (!co_await before(req, res))
        co_return false;

    if (target.empty() && !req.decoded_path().ends_with("/")) {
        res.set_redirect(std::string(req.decoded_path()) + "/");
        co_return co_await after(req, res);
    }

    if (!path.has_filename()) {
        for (const auto& doc_name : default_doc_name_) {
            auto doc_path = path / doc_name;

            boost::system::error_code ec;
            if (!fs::is_regular_file(doc_path, ec))
                continue;

            path = doc_path;
            break;
        }
    }
    if (path.has_filename()) {
        if (fs::is_regular_file(path, ec)) {
            res.set_file_content(path, req.base());
            co_return co_await after(req, res);
        }
    }
    else if (fs::is_directory(path, ec)) {
        beast::error_code ec;
        auto body = html::format_dir_to_html(req.decoded_path(), path, ec);
        if (ec)
            co_return false;
        res.set_string_content(body, "text/html; charset=utf-8");
        co_return co_await after(req, res);
    }
    co_return false;
}
net::awaitable<bool> mount_point_entry::before(request& req, response& res) const
{
    co_return true;
}
net::awaitable<bool> mount_point_entry::after(request& req, response& res) const
{
    co_return true;
}


} // namespace httplib::server
