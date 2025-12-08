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

    enum class dir_format_type
    {
        json,
        html
    };

public:
    const std::string& mount_point() const;
    const fs::path& base_dir() const;

    void set_enabled_dir(bool enabled);
    void set_dir_format(dir_format_type type);
    void set_default_doc_name(const std::vector<std::string>& default_doc_name);

public:
    void operator()(request& req, response& res) const;

private:
    std::string mount_point_;
    fs::path base_dir_;
    bool enabled_dir_         = true;
    dir_format_type dir_type_ = dir_format_type::json;

    std::vector<std::string> default_doc_name_ = {"index.html", "index.htm"};
};

} // namespace httplib::server