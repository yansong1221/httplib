#pragma once
#include "httplib/config.hpp"
#include "httplib/server/server_fwd.hpp"
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

        /// 处理静态文件请求。路径按 request::decoded_segments() 逐段消费：
        /// - 目录存在但 URL 无尾 '/'：301 补齐；若目录列表已关闭且没有默认文档，
        ///   直接返回 404（不做必然通向 403 的重定向，也不暴露目录存在性）；
        /// - 带尾 '/' 时只按目录处理，文件加尾 '/' 返回 404；
        /// - 解码后段内包含 '/'（来自 %2F）时返回 400，不会变成目录层级。
        void operator()(request& req, response& resp) const;

      private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };

} // namespace httplib::server