#include "httplib/server/mount_point_entry.hpp"
#include "html/html.h"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "request_impl.hpp"

namespace httplib::server
{

    struct mount_point_entry::impl
    {
        std::string mount_point;
        fs::path base_dir;
        bool enabled_directory = true;
        mount_point_entry::dir_format_type directory_type = mount_point_entry::dir_format_type::json;
        std::vector<std::string> default_document_name = { "index.html", "index.htm" };
    };

    namespace detail
    {
        inline static bool
        is_valid_path(std::string_view path)
        {
            size_t level = 0;
            size_t i = 0;

            // Skip slash
            while (i < path.size() && path[i] == '/')
            {
                i++;
            }

            while (i < path.size())
            {
                // Read component
                auto beg = i;
                while (i < path.size() && path[i] != '/')
                {
                    if (path[i] == '\0')
                    {
                        return false;
                    }
                    else if (path[i] == '\\')
                    {
                        return false;
                    }
                    i++;
                }

                auto len = i - beg;
                assert(len > 0);

                if (!path.compare(beg, len, "."))
                {
                    ;
                }
                else if (!path.compare(beg, len, ".."))
                {
                    if (level == 0)
                    {
                        return false;
                    }
                    level--;
                }
                else
                {
                    level++;
                }

                // Skip slash
                while (i < path.size() && path[i] == '/')
                {
                    i++;
                }
            }

            return true;
        }

    } // namespace detail

    mount_point_entry::mount_point_entry(std::string const& mount_point, fs::path const& base_dir)
        : impl_(std::make_unique<impl>(mount_point, base_dir))
    {
    }
    mount_point_entry::mount_point_entry(mount_point_entry const& other) : impl_(std::make_unique<impl>(*other.impl_))
    {
    }
    mount_point_entry::mount_point_entry(mount_point_entry&&) noexcept = default;
    mount_point_entry::~mount_point_entry() = default;
    mount_point_entry&
    mount_point_entry::operator=(mount_point_entry const& other)
    {
        if (this != &other)
        {
            impl_ = std::make_unique<impl>(*other.impl_);
        }
        return *this;
    }
    mount_point_entry& mount_point_entry::operator=(mount_point_entry&&) noexcept = default;

    std::string const&
    mount_point_entry::mount_point() const
    {
        return impl_->mount_point;
    }
    httplib::fs::path const&
    mount_point_entry::base_dir() const
    {
        return impl_->base_dir;
    }
    void
    mount_point_entry::operator()(request& req, response& res) const
    {
        auto relative_path = req.path_param<std::string>("*");
        if (!detail::is_valid_path(relative_path))
        {
            res.set_error_content(http::status::bad_request);
            return;
        }

        std::error_code ec;
        auto path = impl_->base_dir
                    / fs::path(std::u8string_view((char8_t const*)relative_path.data(), relative_path.size()));
        if (!fs::exists(path, ec))
        {
            res.set_error_content(http::status::not_found);
            return;
        }

        if (!path.has_filename())
        {
            for (auto const& doc_name : impl_->default_document_name)
            {
                auto doc_path = path / doc_name;

                boost::system::error_code ec;
                if (!fs::is_regular_file(doc_path, ec))
                {
                    continue;
                }

                path = doc_path;
                break;
            }
        }
        if (path.has_filename())
        {
            if (fs::is_regular_file(path, ec))
            {
                res.set_file_content(path, get_impl(req).base());
            }
            else
            {
                res.set_error_content(http::status::not_found);
            }
            return;
        }
        else if (fs::is_directory(path, ec))
        {
            if (impl_->enabled_directory)
            {
                beast::error_code ec;
                switch (impl_->directory_type)
                {
                    case mount_point_entry::dir_format_type::json:
                    {
                        auto doc = html::format_dir_to_json(path, ec);
                        res.set_json_content(std::move(doc));
                    }
                    break;
                    case mount_point_entry::dir_format_type::html:
                    {
                        auto body = html::format_dir_to_html(req.path(), path, ec);
                        res.set_string_content(body, "text/html; charset=utf-8");
                    }
                    break;
                    default:
                        break;
                }
                if (ec)
                {
                    res.set_error_content(http::status::internal_server_error);
                }
            }
            else
            {
                res.set_error_content(http::status::forbidden);
            }
            return;
        }
        res.set_error_content(http::status::forbidden);
    }

    void
    mount_point_entry::set_enabled_directory(bool enabled)
    {
        impl_->enabled_directory = enabled;
    }

    void
    mount_point_entry::set_directory_format(dir_format_type type)
    {
        impl_->directory_type = type;
    }

    void
    mount_point_entry::set_default_document_name(std::vector<std::string> const& default_document_name)
    {
        impl_->default_document_name = default_document_name;
    }

} // namespace httplib::server
