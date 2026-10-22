#pragma once
#include "httplib/url/url.hpp"
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace httplib::server::detail
{

    /// 按原始路径中的字面 '/' 严格分段：不 trim、不合并重复分隔符。
    /// 段边界在解码前确定，因此 "%2F" 不会参与分段；尾随 '/' 会产生一个空段，
    /// 与路由注册 key 的切分规则保持一致。根路径 "/" 表示为一个空段。
    inline std::vector<std::string_view>
    split_path_segments(std::string_view path)
    {
        std::vector<std::string_view> out;

        if (path.empty())
        {
            return out;
        }

        // 只去掉开头一个分隔符；后续连续 '/' 都是空段。
        if (path.front() == '/')
        {
            path.remove_prefix(1);
        }

        if (path.empty())
        {
            out.emplace_back();
            return out;
        }

        std::size_t begin = 0;
        for (;;)
        {
            auto const slash = path.find('/', begin);
            if (slash == std::string_view::npos)
            {
                out.push_back(path.substr(begin));
                break;
            }

            out.push_back(path.substr(begin, slash - begin));
            begin = slash + 1;

            if (begin == path.size())
            {
                out.emplace_back();
                break;
            }
        }

        return out;
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
