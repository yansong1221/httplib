#pragma once
#include "beast_alias.hpp"
#include <boost/asio/ip/address.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace httplib::server
{

    /// 可信代理来源集合（CDN / nginx / 另一台 httplib 服务器等）。
    ///
    /// 存在的理由：`X-Forwarded-For` 由客户端任意填写，只有当直连对端本身属于
    /// 可信集合时，它声明的地址才可信。否则任何客户端都能自己声明 IP——按 IP
    /// 限流、审计日志都会被凭空伪造。
    ///
    /// 一经构造即不可变，由 server 共享给每个 request::impl 读；调用
    /// set_trusted_proxies() 只是整体换掉这个 shared_ptr，已在处理的请求继续
    /// 使用旧集合，不会看到撕裂的中间状态。
    class HTTPLIB_API trusted_proxies
    {
      public:
        /// @param cidrs CIDR（如 "10.0.0.0/8"）或单个地址（如 "192.0.2.7"）。
        ///              非法输入抛 std::invalid_argument。空列表表示不信任任何代理。
        explicit trusted_proxies(std::vector<std::string> const& cidrs);

        /// 该地址是否属于可信代理来源。
        [[nodiscard]] bool contains(net::ip::address const& addr) const;

      private:
        /// 网络地址 + 前缀长度。用 net::ip::address 存 v4/v6 两种地址族，
        /// 匹配时按族区分，一条代码路径覆盖两种情况。
        struct cidr
        {
            net::ip::address network;
            unsigned short prefix_length = 0;
        };

        std::vector<cidr> cidrs_;
    };

} // namespace httplib::server
