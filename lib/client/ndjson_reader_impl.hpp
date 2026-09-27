#pragma once
#include "response_impl.h"
#include <array>
#include <boost/json/parse.hpp>
#include <boost/system/result.hpp>
#include <string>
#include <string_view>

namespace httplib::client
{

    class ndjson_reader_impl : public httplib::client::ndjson_reader
    {
      public:
        explicit ndjson_reader_impl(std::shared_ptr<response::impl> impl) : impl_(std::move(impl)) {}

        net::awaitable<boost::system::result<boost::json::value>>
        read() override
        {
            boost::system::error_code ec;

            for (;;)
            {
                auto lf = buf_.find('\n');
                if (lf == std::string::npos)
                {
                    if (!impl_->is_body_done())
                    {
                        auto bytes = co_await impl_->read_some_decompressed(net::buffer(read_buf_), ec);
                        if (ec)
                        {
                            co_return ec;
                        }
                        if (bytes != 0)
                        {
                            buf_.append(read_buf_.data(), bytes);
                        }
                        continue;
                    }

                    co_return finish_tail();
                }

                auto line = buf_.substr(0, lf);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                buf_.erase(0, lf + 1);

                if (line.empty())
                {
                    continue;
                }

                co_return parse_line(line);
            }
        }

        bool
        is_done() const override
        {
            return impl_->is_body_done() && buf_.empty();
        }

      private:
        /// 流到尽头而 buf_ 里还有残留时，把残留当作最后一条记录交出，并清空 buf_ 使
        /// is_done() 得以成立。末行不带换行是合法的（RFC 7464 §2 允许最后一行省略结尾
        /// 换行），本库之外的 NDJSON 生产者也很常见这么写。
        ///
        /// 旧实现在这个位置直接返回空 value，副作用有两个，且都不会报错：
        ///   1. 残留整段被丢掉——末条记录静默消失。tests/common.hpp 的 collect_ndjson_lines
        ///      正是 while(!is_done()) + 读到 null 就 break，于是调用方拿到的是一个"干净的"
        ///      短结果流，无从察觉少了一条。
        ///   2. buf_ 不清空，于是 is_done()（body 结束且 buf_ 为空）永远为 false：只按
        ///      is_done() 推进、不检查 null 的调用方会一直读到空 value，停不下来。
        ///
        /// 语义对齐同族的 sse_reader：真正没有记录时才返回空 value 作结束信号。
        boost::system::result<boost::json::value>
        finish_tail()
        {
            auto line = std::move(buf_);
            buf_.clear();
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.empty())
            {
                return boost::json::value {};
            }
            // 残留不是完整 JSON（流被截断）就如实报错，不静默当成"没有记录"。
            return parse_line(line);
        }

        /// 解析一行并把失败以 error_code 报回去。
        ///
        /// read() 的返回类型是 boost::system::result<value>，即"失败走 error_code"的契约，
        /// 所以这里必须用 boost::json::parse 的非抛异常重载。旧实现两处都用抛异常重载：
        /// 坏行会把 boost::system::system_error 抛穿协程，而不是返回 error——于是调用方按契约
        /// 写的 result.has_error() 对解析失败而言是死代码（tests/common.hpp 的
        /// collect_ndjson_lines 就先判 has_error()），而数据损坏表现为一个意外异常。
        /// lib/db/row.cpp 用的正是这个非抛异常形式。
        boost::system::result<boost::json::value>
        parse_line(std::string_view line)
        {
            boost::system::error_code ec;
            // 非抛异常重载的签名是 value parse(string_view, system::error_code&)，失败经 ec
            // 报告、直接返回 value（不是 result<value>）。
            auto parsed = boost::json::parse(line, ec);
            if (ec)
            {
                return ec;
            }
            return std::move(parsed);
        }

        std::shared_ptr<response::impl> impl_;
        std::string buf_;
        std::array<char, 4096> read_buf_;
    };

} // namespace httplib::client
