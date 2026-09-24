#pragma once
#include "compress/compressor.hpp"
#include "httplib/config.hpp"
#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/result.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace httplib::body
{
    /** 流式 Content-Encoding 解码器。

        逐块喂入线上压缩字节（`feed`），逐块取出解压结果（`drain`）。`identity` / 未知编码
        退化为纯透传（`transforms() == false`），此时 `feed` 的字节原样进内部缓冲。

        解压炸弹防护：`limit > 0` 时对**产出**字节计数，超过报 `http::error::body_limit`；
        与旧 `compressed_body` 一致，压缩流会先从限额里扣除 `Content-Length`（压缩长度）。

        解压结果放内部缓冲，调用方缓冲放不下的部分保留到下次 `drain`，不丢数据。
    */
    class stream_decoder
    {
      public:
        stream_decoder() = default;

        void reset(std::string_view encoding,
                   std::optional<std::uint64_t> const& content_length,
                   std::uint64_t limit,
                   boost::system::error_code& ec);

        bool
        transforms() const
        {
            return compressor_ != nullptr;
        }

        /// 内部尚未取走的解压字节数。
        std::size_t
        buffered() const
        {
            return out_.size() - offset_;
        }

        /// 喂入一块线上字节；解压结果进入内部缓冲。
        void feed(net::const_buffer const& raw, boost::system::error_code& ec);

        /// 把内部缓冲拷进 dst，返回拷出字节数（0 表示暂无）。
        std::size_t drain(net::mutable_buffer const& dst, boost::system::error_code& ec);

        /// 输入结束：冲刷解压器，残余结果留在内部缓冲供 `drain` 取。
        void flush(boost::system::error_code& ec);

      private:
        void pump(boost::system::error_code& ec);
        void append(char const* data, std::size_t size);
        void account(std::size_t size, boost::system::error_code& ec);

        compress::compressor::ptr compressor_;
        std::string out_;
        std::size_t offset_ = 0;
        std::uint64_t limit_ = 0;
        std::uint64_t produced_ = 0;
        bool finished_ = false;
    };

    /** 流式 Content-Encoding 编码器（写方向的对称实现）。

        `feed` 喂入明文块，`buffer` 暴露压缩产物，`consume_all` 在产物被取走后推进。
    */
    class stream_encoder
    {
      public:
        stream_encoder() = default;

        void reset(std::string_view encoding, boost::system::error_code& ec);

        bool
        transforms() const
        {
            return compressor_ != nullptr;
        }

        void feed(net::const_buffer const& plain, bool more, boost::system::error_code& ec);

        /// 当前可取的压缩产物（消费前有效）。
        net::const_buffer
        buffer() const
        {
            return { out_.data() + offset_, out_.size() - offset_ };
        }

        void
        consume_all()
        {
            out_.clear();
            offset_ = 0;
        }

        /// 明文结束：冲刷编码器，残余产物留在内部缓冲供 `buffer` 取。
        void flush(boost::system::error_code& ec);

      private:
        void pump(boost::system::error_code& ec);

        compress::compressor::ptr compressor_;
        std::string out_;
        std::size_t offset_ = 0;
        bool finished_ = false;
    };

    /// 一次性解压，`encoding` 为 identity/未知时原样返回。
    boost::system::result<std::string> decode(std::string_view bytes, std::string_view encoding, std::uint64_t limit = 0);

    /// 一次性压缩，`encoding` 为 identity/未知时原样返回。
    boost::system::result<std::string> encode(std::string_view bytes, std::string_view encoding);

} // namespace httplib::body
