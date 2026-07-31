#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
#include <boost/asio/awaitable.hpp>
#include <filesystem>

namespace httplib::server {

class HTTPLIB_API mount_point_entry
{
public:
    mount_point_entry(const std::string& _mount_point, const fs::path& _base_dir);
    virtual ~mount_point_entry() = default;

    enum class dir_format_type
    {
        json,
        html
    };

public:
    const std::string& mount_point() const;
    const fs::path& base_dir() const;

    void set_enabled_directory(bool enabled);
    void set_directory_format(dir_format_type type);
    void set_default_document_name(const std::vector<std::string>& default_document_name);

public:
    void operator()(request& req, response& resp) const;

private:
    std::string mount_point_;
    fs::path base_dir_;
    bool enabled_directory_           = true;
    dir_format_type directory_type_   = dir_format_type::json;

    std::vector<std::string> default_document_name_ = {"index.html", "index.htm"};
};

} // namespace httplib::server