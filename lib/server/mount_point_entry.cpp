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
void mount_point_entry::operator()(request& req, response& res) const
{
    auto relative_path = req.path_param("*");
    if (!detail::is_valid_path(relative_path)) {
        res.set_error_content(http::status::bad_request);
        return;
    }

    std::error_code ec;
    auto path = base_dir_ / fs::path(std::u8string_view((const char8_t*)relative_path.data(),
                                                        relative_path.size()));
    if (!fs::exists(path, ec)) {
        res.set_error_content(http::status::not_found);
        return;
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
            return;
        }
    }
    else if (fs::is_directory(path, ec)) {
        beast::error_code ec;
        auto body = html::format_dir_to_html(req.decoded_path(), path, ec);
        if (ec)
            res.set_error_content(http::status::internal_server_error);
        else
            res.set_string_content(body, "text/html; charset=utf-8");
        return;
    }
    res.set_error_content(http::status::not_found);
}


} // namespace httplib::server
