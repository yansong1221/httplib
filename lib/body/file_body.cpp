#include "httplib/body/file_body.hpp"
#include <fmt/format.h>

namespace httplib::body {

file_body::writer::writer(const http::fields&, value_type& b)
    : body_(b)
{
}

void file_body::writer::init(boost::system::error_code& ec)
{
    ec.clear();
}

boost::optional<std::pair<file_body::writer::const_buffers_type, bool>>
file_body::writer::get(boost::system::error_code& ec)
{
    if (body_.ranges.size() == 1 || body_.ranges.empty()) {
        html::http_ranges::range_type range;
        if (body_.ranges.empty())
            range = {0, body_.file_size()};
        else {
            range        = body_.ranges.ranges().front();
            range.second = range.second + 1;
        }

        if (!pos_) {
            pos_ = range.first;
            body_.seekg(*pos_);
        }
        std::size_t const n = (std::min)(sizeof(buf_), beast::detail::clamp(range.second - *pos_));
        if (n == 0) {
            ec = {};
            return boost::none;
        }
        auto const nread = body_.read(buf_, n);
        if (nread == 0) {
            ec = http::error::short_read;
            return boost::none;
        }
        BOOST_ASSERT(nread != 0);
        *pos_ += nread;
        ec = {};

        return {{{buf_, nread},          // buffer to return.
                 *pos_ < range.second}}; // `true` if there are more buffers.
    }

    if (!range_index_) {
        range_index_ = 0;
        step_        = step::header;
    }

    if (*range_index_ >= body_.ranges.size()) {
        ec = {};
        return boost::none;
    }
    const auto& range = body_.ranges.at(*range_index_);
    switch (step_) {
        case step::header: {
            std::string header = fmt::format("--{}\r\n", body_.boundary);
            header += fmt::format("Content-Type: {}\r\n", body_.content_type);
            header += fmt::format(
                "Content-Range: bytes {}-{}/{}\r\n", range.first, range.second, file_size_);
            header += "\r\n";
            strcpy(buf_, header.c_str());
            step_ = step::content;
            pos_  = std::nullopt;
            return {{{buf_, header.size()}, true}};
        } break;
        case step::content: {
            if (!pos_) {
                pos_ = range.first;
                body_.seekg(*pos_);
            }
            std::size_t const n =
                (std::min)(sizeof(buf_), beast::detail::clamp(range.second - *pos_));
            if (n == 0) {
                step_ = step::content_end;
                return get(ec);
            }
            auto const nread = body_.read(buf_, n);
            if (nread == 0) {
                ec = http::error::short_read;
                return boost::none;
            }
            BOOST_ASSERT(nread != 0);
            *pos_ += nread;
            ec = {};

            return {{{buf_, nread}, true}};
        } break;
        case step::content_end: {
            bool is_eof = (*range_index_) == body_.ranges.size() - 1;
            std::string end("\r\n");
            if (is_eof) {
                end += fmt::format("--{}--\r\n", body_.boundary);
                step_ = step::eof;
            }
            else {
                step_ = step::header;
                (*range_index_)++;
            }
            net::buffer_copy(net::buffer(buf_, sizeof(buf_)), net::buffer(end));
            return std::make_pair<net::const_buffer>(net::buffer(buf_, end.size()), !is_eof);
        } break;
        case step::eof: break;
        default: break;
    }

    return boost::none;
}

file_body::reader::reader(const http::fields&, value_type& b)
    : body_(b)
{
}

void file_body::reader::init(boost::optional<std::uint64_t> const& content_length,
                             boost::system::error_code& ec)
{
}

void file_body::reader::finish(boost::system::error_code& ec)
{
    ec.clear();
}

} // namespace httplib::body