#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include <boost/asio/awaitable.hpp>
#include <filesystem>
#include <memory>

namespace httplib::server
{

    class HTTPLIB_API mount_point_entry
    {
      public:
        mount_point_entry(std::string const& mount_point, fs::path const& base_dir);
        mount_point_entry(mount_point_entry const& other);
        mount_point_entry(mount_point_entry&&) noexcept;
        mount_point_entry& operator=(mount_point_entry const& other);
        mount_point_entry& operator=(mount_point_entry&&) noexcept;
        ~mount_point_entry();

        enum class dir_format_type
        {
            json,
            html
        };

      public:
        std::string const& mount_point() const;
        fs::path const& base_dir() const;

        void set_enabled_directory(bool enabled);
        void set_directory_format(dir_format_type type);
        void set_default_document_name(std::vector<std::string> const& default_document_name);

        void operator()(request& req, response& resp) const;

      private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server