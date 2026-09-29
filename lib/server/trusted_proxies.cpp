#include "trusted_proxies.hpp"
#include <algorithm>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/ip/network_v4.hpp>
#include <boost/asio/ip/network_v6.hpp>
#include <boost/system/error_code.hpp>
#include <cstddef>
#include <stdexcept>

namespace httplib::server
{
    namespace
    {
        /// (candidate & mask) == (network & mask)，mask 由 prefix_length 逐字节推出。
        /// 直接比前缀而不是生成 netmask：boost 的 network_v4 有 netmask() 而
        /// network_v6 没有，这里用统一的字节比较避开这个不对称。
        template <typename Address>
        bool
        prefix_matches_impl(Address const& network, unsigned short prefix_length, Address const& candidate)
        {
            auto const net_bytes = network.to_bytes();
            auto const cand_bytes = candidate.to_bytes();

            auto bits_left = static_cast<unsigned>(prefix_length);
            for (std::size_t i = 0; i < net_bytes.size(); ++i)
            {
                auto const take = std::min(8u, bits_left);
                auto const mask = static_cast<unsigned char>(take == 0 ? 0u : (0xFFu << (8 - take)) & 0xFFu);
                if ((cand_bytes[i] & mask) != (net_bytes[i] & mask))
                {
                    return false;
                }
                bits_left -= take;
            }
            return true;
        }

        /// net::ip::address 是 v4/v6 的变体包装，to_bytes() 在具体类型上；先按族
        /// 分派，再走同一份前缀比较逻辑。
        bool
        prefix_matches(net::ip::address const& network, unsigned short prefix_length, net::ip::address const& candidate)
        {
            if (network.is_v4())
            {
                return prefix_matches_impl(network.to_v4(), prefix_length, candidate.to_v4());
            }
            return prefix_matches_impl(network.to_v6(), prefix_length, candidate.to_v6());
        }
    } // namespace

    trusted_proxies::trusted_proxies(std::vector<std::string> const& cidrs)
    {
        cidrs_.reserve(cidrs.size());

        for (auto const& cidr : cidrs)
        {
            boost::system::error_code ec;

            if (cidr.find('/') != std::string::npos)
            {
                if (auto net4 = net::ip::make_network_v4(cidr, ec); !ec)
                {
                    cidrs_.push_back({ net4.address(), net4.prefix_length() });
                    continue;
                }
                ec.clear();
                if (auto net6 = net::ip::make_network_v6(cidr, ec); !ec)
                {
                    cidrs_.push_back({ net6.address(), net6.prefix_length() });
                    continue;
                }
            }
            else
            {
                // 单个地址按主机路由处理：v4 等价 /32，v6 等价 /128。
                if (auto addr4 = net::ip::make_address_v4(cidr, ec); !ec)
                {
                    cidrs_.push_back({ addr4, 32 });
                    continue;
                }
                ec.clear();
                if (auto addr6 = net::ip::make_address_v6(cidr, ec); !ec)
                {
                    cidrs_.push_back({ addr6, 128 });
                    continue;
                }
            }

            throw std::invalid_argument("invalid trusted proxy CIDR or address: " + cidr);
        }
    }

    bool
    trusted_proxies::contains(net::ip::address const& addr) const
    {
        for (auto const& c : cidrs_)
        {
            // 地址族不同不可能落在同一网段内，显式跳过而不是让字节数不同导致误判。
            if (c.network.is_v4() != addr.is_v4())
            {
                continue;
            }
            if (prefix_matches(c.network, c.prefix_length, addr))
            {
                return true;
            }
        }
        return false;
    }

} // namespace httplib::server
