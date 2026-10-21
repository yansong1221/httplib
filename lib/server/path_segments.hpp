#pragma once
#include "httplib/url/url.hpp"
#include "httplib/util/misc.hpp"
#include <string>
#include <string_view>
#include <vector>

namespace httplib::server::detail
{

    /// 按原始路径中的字面 '/' 分段。段边界在解码前确定，因此 "%2F" 不会参与
    /// 分段；尾随 '/' 会产生一个空段，与路由注册 key 的切分规则保持一致。
    inline std::vector<std::string_view>
    split_path_segments(std::string_view path)
    {
        auto segments = util::split(path, "/");

        if (path.ends_with("/"))
        {
            segments.push_back(std::string_view());
        }

        return segments;
    }

    /// 按字面 '/' 分段后逐段解码。返回的段值可以包含 '/'（来自 "%2F"），
    /// 但原始段边界不会因此丢失。
    inline std::vector<std::string>
    decode_path_segments(std::string_view path)
    {
        auto raw = split_path_segments(path);

        std::vector<std::string> out;
        out.reserve(raw.size());

        for (auto seg : raw)
        {
            if (seg.find('%') == std::string_view::npos)
            {
                out.emplace_back(seg);
            }
            else
            {
                out.emplace_back(url::url_decode(seg));
            }
        }
        return out;
    }

} // namespace httplib::server::detail
