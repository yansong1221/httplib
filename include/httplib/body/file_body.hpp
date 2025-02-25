#pragma once
#include "httplib/config.hpp"
#include "httplib/util/misc.hpp"
#include "httplib/html.hpp"
#include <boost/beast/core/file.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/message.hpp>
#include <fmt/format.h>
#include <fstream>

namespace httplib::body
{
struct file_body
{
    struct value_type
    {
        html::http_ranges ranges;
        std::string content_type;
        std::string boundary;

        std::size_t file_size() const { return file_size_; }

        void seekg(std::ios::off_type _Off, std::ios_base::seekdir _Way = std::ios::cur) { file_.seekg(_Off, _Way); }
        std::size_t read(void* buffer, std::size_t n)
        {
            file_.read((char*)buffer, n);
            return file_.gcount();
        }
        void open(const fs::path& path, std::ios_base::openmode mode = std::ios_base::in | std::ios_base::out)
        {
            file_size_ = 0;
            file_.open(path, mode);
            if (file_)
            {
                file_.seekg(0, std::ios::end);
                file_size_ = file_.tellg();
                file_.seekg(0, std::ios::beg);
            }
        }
        bool is_open() const { return file_.is_open(); }

    private:
        std::fstream file_;
        std::size_t file_size_ = 0;
    };

    class writer
    {
    public:
        using const_buffers_type = net::const_buffer;

        template<bool isRequest, class Fields>
        writer(http::header<isRequest, Fields>& h, value_type& b) : body_(b)
        {
        }

        void init(boost::system::error_code& ec) { ec.clear(); }
        inline boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec)
        {
            if (body_.ranges.size() == 1 || body_.ranges.empty())
            {
               html::range_type range;
                if (body_.ranges.empty())
                    range = {0, body_.file_size()};
                else
                {
                    range = body_.ranges.front();
                    range.second = range.second + 1;
                }

                if (!pos_)
                {
                    pos_ = range.first;
                    body_.seekg(*pos_);
                }
                std::size_t const n = (std::min)(sizeof(buf_), beast::detail::clamp(range.second - *pos_));
                if (n == 0)
                {
                    ec = {};
                    return boost::none;
                }
                auto const nread = body_.read(buf_, n);
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
                    std::string header = fmt::format("--{}\r\n", body_.boundary);
                    header += fmt::format("Content-Type: {}\r\n", body_.content_type);
                    header += fmt::format("Content-Range: bytes {}-{}/{}\r\n", range.first, range.second, file_size_);
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
                        body_.seekg(*pos_);
                    }
                    std::size_t const n = (std::min)(sizeof(buf_), beast::detail::clamp(range.second - *pos_));
                    if (n == 0)
                    {
                        step_ = step::content_end;
                        return get(ec);
                    }
                    auto const nread = body_.read(buf_, n);
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
                        end += fmt::format("--{}--\r\n", body_.boundary);
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
        std::size_t file_size_;
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