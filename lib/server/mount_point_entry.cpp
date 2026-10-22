#include "httplib/server/mount_point_entry.hpp"
#include "beast_alias.hpp"
#include "html/html.h"
#include "httplib/server/request.hpp"
#include "httplib/server/response.hpp"
#include "path_segments.hpp"
#include "request_impl.hpp"

namespace httplib::server
{

    struct mount_point_entry::impl
    {
        std::string mount_point;
        fs::path base_dir;
        std::vector<std::string> mount_segments;
        bool enabled_directory = true;
        mount_point_entry::dir_format_type directory_type = mount_point_entry::dir_format_type::json;
        std::vector<std::string> default_document_name = { "index.html", "index.htm" };
    };

    namespace detail
    {
        /// mount point 的段按注册 key 的规则归一：只去掉首尾 '/'，段内空格原样保留。
        /// "/" 与空串都表示根，返回空段列表。
        inline std::vector<std::string>
        split_mount_point(std::string_view mount_point)
        {
            while (!mount_point.empty() && mount_point.front() == '/')
            {
                mount_point.remove_prefix(1);
            }
            while (!mount_point.empty() && mount_point.back() == '/')
            {
                mount_point.remove_suffix(1);
            }
            if (mount_point.empty())
            {
                return {};
            }

            auto raw = split_path_segments(mount_point);
            std::vector<std::string> out;
            out.reserve(raw.size());
            for (auto seg : raw)
            {
                out.emplace_back(seg);
            }
            return out;
        }

        /// 目录下是否存在任一默认文档。用于判断补尾斜杠后是否还有内容可服务：
        /// 关闭目录列表且没有默认文档时，301 之后必然 403，应直接返回 403。
        inline bool
        has_default_document(fs::path const& dir, std::vector<std::string> const& names)
        {
            std::error_code ec;
            for (auto const& name : names)
            {
                if (fs::is_regular_file(dir / name, ec))
                {
                    return true;
                }
            }
            return false;
        }

        inline static bool
        is_within(fs::path const& path, fs::path const& base_dir, std::error_code& ec)
        {
            auto canonical_base = fs::weakly_canonical(base_dir, ec);
            if (ec)
            {
                return false;
            }
            auto canonical_path = fs::weakly_canonical(path, ec);
            if (ec)
            {
                return false;
            }
            auto base = canonical_base.string();
            auto resolved = canonical_path.string();
            if (resolved.size() < base.size())
            {
                return false;
            }
            if (resolved.size() == base.size())
            {
                return resolved == base;
            }
            if (resolved[base.size()] == fs::path::preferred_separator)
            {
                return true;
            }
            // 尾带分隔符的根路径（如驱动器根 `C:\`），其下任意路径均在界内
            return base.size() > 0 && base.back() == fs::path::preferred_separator && resolved.rfind(base, 0) == 0;
        }

    } // namespace detail

    mount_point_entry::mount_point_entry(std::string const& mount_point, fs::path const& base_dir)
        : impl_(std::make_unique<impl>(mount_point, base_dir))
    {
        impl_->mount_segments = detail::split_mount_point(impl_->mount_point);
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
        auto const segments = req.decoded_segments();
        auto const& mount_segments = impl_->mount_segments;

        if (segments.size() < mount_segments.size())
        {
            res.set_error_content(status::not_found);
            return;
        }
        for (size_t i = 0; i < mount_segments.size(); ++i)
        {
            if (segments[i] != mount_segments[i])
            {
                res.set_error_content(status::not_found);
                return;
            }
        }

        auto const relative = segments.subspan(mount_segments.size());
        // 尾随字面 '/' 会保留成一个空段。以它作为“这是一个目录请求”的显式信号，
        // 不再依赖 fs::path::has_filename() 的平台细节。裸挂载点（relative 为空）
        // 与普通目录一样先按“无尾斜杠的目录”处理，走下面的 301/403 判定。
        bool const dir_requested = !relative.empty() && relative.back().empty();

        // 直接用段构造 fs::path，避免先把段拼回字符串再解析。
        // level 只用于拒绝越过 mount 根的 ".."；内部 "a/../b" 仍交给
        // weakly_canonical() 解析，保持原有语义。
        size_t level = 0;
        fs::path relative_path;
        for (auto const& seg : relative)
        {
            // 段内出现 '/' 只可能来自 %2F；它是段内容，不能重新变成目录层级。
            if (seg.find('/') != std::string::npos || seg.find('\\') != std::string::npos
                || seg.find('\0') != std::string::npos)
            {
                res.set_error_content(status::bad_request);
                return;
            }

            // 空段只表示尾斜杠或重复斜杠；尾斜杠已由 dir_requested 单独记录。
            if (seg.empty() || seg == ".")
            {
                continue;
            }

            if (seg == "..")
            {
                if (level == 0)
                {
                    res.set_error_content(status::bad_request);
                    return;
                }
                --level;
            }
            else
            {
                ++level;
            }

            relative_path /= fs::path(std::u8string_view((char8_t const*)seg.data(), seg.size()));
        }

        std::error_code ec;
        auto path = impl_->base_dir / relative_path;
        if (!detail::is_within(path, impl_->base_dir, ec))
        {
            res.set_error_content(status::forbidden);
            return;
        }
        if (!fs::exists(path, ec))
        {
            res.set_error_content(status::not_found);
            return;
        }

        if (!dir_requested)
        {
            if (fs::is_regular_file(path, ec))
            {
                res.set_file_content(path, req.base());
                return;
            }
            if (fs::is_directory(path, ec))
            {
                // 目录列表已关闭且没有默认文档时，补完斜杠也只会 403。这里不做
                // 无意义的重定向；同时无尾斜杠的 URL 本身不指向可服务的文件，
                // 因此按 not_found 处理，避免用 403 暴露目录存在性。
                if (!impl_->enabled_directory
                    && !detail::has_default_document(path, impl_->default_document_name))
                {
                    res.set_error_content(status::not_found);
                    return;
                }

                // 目录存在才补尾斜杠；不存在或普通文件不会走到这里。
                std::string location(req.path());
                location += '/';

                auto const target = req.target();
                if (auto const q = target.find('?'); q != std::string_view::npos)
                {
                    location.append(target.substr(q));
                }

                res.set_redirect(location, status::moved_permanently);
                return;
            }
            res.set_error_content(status::not_found);
            return;
        }

        if (!fs::is_directory(path, ec))
        {
            // 例如 /file.txt/：尾斜杠明确要求目录，不能回退成文件。
            res.set_error_content(status::not_found);
            return;
        }

        for (auto const& doc_name : impl_->default_document_name)
        {
            auto doc_path = path / doc_name;
            if (fs::is_regular_file(doc_path, ec))
            {
                res.set_file_content(doc_path, req.base());
                return;
            }
        }

        if (!impl_->enabled_directory)
        {
            res.set_error_content(status::forbidden);
            return;
        }

        beast::error_code html_ec;
        switch (impl_->directory_type)
        {
            case mount_point_entry::dir_format_type::json:
            {
                auto doc = html::format_dir_to_json(path, html_ec);
                res.set_json_content(std::move(doc));
            }
            break;
            case mount_point_entry::dir_format_type::html:
            {
                // 挂载根目录没有 mount point 内的父目录：生成 "../" 会让浏览器
                // 跳到宿主 URL 空间（例如 /static/ 的上一级是 /）。
                auto body = html::format_dir_to_html(req.path(), path, html_ec, !relative_path.empty());
                res.set_string_content(body, "text/html; charset=utf-8");
            }
            break;
            default:
                break;
        }
        if (html_ec)
        {
            res.set_error_content(status::internal_server_error);
        }
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
