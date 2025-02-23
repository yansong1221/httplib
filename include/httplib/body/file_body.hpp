#pragma once
#include "httplib/config.hpp"
#include "httplib/util/misc.hpp"
#include <boost/beast/core/file.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/message.hpp>
#include <fmt/format.h>

namespace httplib::body
{
struct file_body
{
    struct value_type : public beast::file
    {
        using range_type = std::pair<int64_t, int64_t>;

        std::vector<range_type> ranges;
        std::string content_type;
    };

    class writer
    {
    public:
        using const_buffers_type = net::const_buffer;

        template<bool isRequest, class Fields>
        writer(http::header<isRequest, Fields>& h, value_type& b) : body_(b)
        {
            if constexpr (!isRequest)
            {
                h.result(body_.ranges.empty() ? http::status::ok : http::status::partial_content);
                if (body_.ranges.size() == 1)
                {
                    boost::system::error_code ec;
                    const auto& range = body_.ranges.front();
                    h.set(http::field::content_range,
                          fmt::format("bytes {}-{}/{}", range.first, range.second, body_.size(ec)));
                    h.set(http::field::content_type, body_.content_type);
                }
                else if (!body_.ranges.empty())
                {
                    boundary_ = util::generate_boundary();
                    h.set(http::field::content_type, fmt::format("multipart/byteranges; boundary={}", boundary_));
                }

                h.set(http::field::accept_ranges, "bytes");
            }
        }

        void init(boost::system::error_code& ec) { ec.clear(); }
        inline boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec)
        {
            if (body_.ranges.size() == 1 || body_.ranges.empty())
            {
                value_type::range_type range;
                if (body_.ranges.empty())
                    range = {0, body_.size(ec)};
                else
                    range = body_.ranges.front();

                if (!pos_)
                {
                    pos_ = range.first;
                    body_.seek(*pos_, ec);
                    if (ec) return boost::none;
                }
                std::size_t const n = (std::min)(sizeof(buf_), beast::detail::clamp(range.second - *pos_));
                if (n == 0)
                {
                    ec = {};
                    return boost::none;
                }
                auto const nread = body_.read(buf_, n, ec);
                if (ec) return boost::none;
                if (nread == 0)
                {
                    ec = http::error::short_read;
                    return boost::none;
                }
                BOOST_ASSERT(nread != 0);
                *pos_ += nread;
                ec = {};

                return {{{buf_, nread},          // buffer to return.
                         *pos_ < range.second}}; // `true` if there are more buffers.
            }

            if (!range_index_)
            {
                range_index_ = 0;
                step_ = step::header;
            }

            if (*range_index_ >= body_.ranges.size())
            {
                ec = {};
                return boost::none;
            }
            auto& range = body_.ranges[*range_index_];
            switch (step_)
            {
                case step::header:
                {
                    std::string header = fmt::format("--{}\r\n", boundary_);
                    header += fmt::format("Content-Type: {}\r\n", body_.content_type);
                    header +=
                        fmt::format("Content-Range: bytes {}-{}/{}\r\n", range.first, range.second, body_.size(ec));
                    header += "\r\n";
                    strcpy(buf_, header.c_str());
                    step_ = step::content;
                    pos_ = std::nullopt;
                    return {{{buf_, header.size()}, true}};
                }
                break;
                case step::content:
                {
                    if (!pos_)
                    {
                        pos_ = range.first;
                        body_.seek(*pos_, ec);
                        if (ec) return boost::none;
                    }
                    std::size_t const n = (std::min)(sizeof(buf_), beast::detail::clamp(range.second - *pos_));
                    if (n == 0)
                    {
                        step_ = step::content_end;
                        return get(ec);
                    }
                    auto const nread = body_.read(buf_, n, ec);
                    if (ec) return boost::none;
                    if (nread == 0)
                    {
                        ec = http::error::short_read;
                        return boost::none;
                    }
                    BOOST_ASSERT(nread != 0);
                    *pos_ += nread;
                    ec = {};

                    return {{{buf_, nread}, true}};
                }
                break;
                case step::content_end:
                {
                    bool is_eof = (*range_index_) == body_.ranges.size() - 1;
                    std::string end("\r\n");
                    if (is_eof)
                    {
                        end += fmt::format("--{}--\r\n", boundary_);
                        step_ = step::eof;
                    }
                    else
                    {
                        step_ = step::header;
                        (*range_index_)++;
                    }
                    net::buffer_copy(net::buffer(buf_, sizeof(buf_)), net::buffer(end));
                    return std::make_pair<net::const_buffer>(net::buffer(buf_, end.size()), !is_eof);
                }
                break;
                case step::eof: break;
                default: break;
            }

            return boost::none;
        }

    private:
        value_type& body_;
        std::string boundary_;

        std::optional<int> range_index_;
        std::optional<std::uint64_t> pos_;
        enum class step
        {
            header,
            content,
            content_end,
            eof
        };
        step step_ = step::header;
        char buf_[BOOST_BEAST_FILE_BUFFER_SIZE];
    };
    //--------------------------------------------------------------------------

    class reader
    {
    public:
        using const_buffers_type = net::const_buffer;

        template<bool isRequest, class Fields>
        reader(http::header<isRequest, Fields>& h, value_type& b)
        {
        }

        inline void init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec) { }

        inline std::size_t put(const_buffers_type const& buffers, boost::system::error_code& ec) { }

        void finish(boost::system::error_code& ec) { ec.clear(); }

    private:
        value_type& body_;
    };
};
} // namespace httplib::body