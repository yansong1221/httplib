#include "body/source.hpp"
#include <boost/beast/core/detail/clamp.hpp>
#include <boost/beast/http/error.hpp>
#include <fmt/format.h>

namespace httplib::body
{
    encoded_source::encoded_source(source_ptr inner, std::string_view encoding) : inner_(std::move(inner))
    {
        boost::system::error_code ec;
        encoder_.reset(encoding, ec);
        if (ec)
        {
            // 编码器初始化失败：退化为透传，错误在首次 next 时由内层/调用方处理。
            flushed_ = true;
        }
    }

    source::chunk_t
    encoded_source::next(boost::system::error_code& ec)
    {
        ec = {};
        if (!encoder_.transforms())
        {
            return inner_->next(ec);
        }

        // 上一块产物已被序列化器消费，推进编码器输出。
        encoder_.consume_all();

        for (;;)
        {
            if (inner_done_)
            {
                if (!flushed_)
                {
                    encoder_.flush(ec);
                    flushed_ = true;
                    if (ec)
                    {
                        return std::nullopt;
                    }
                }
                auto buffer = encoder_.buffer();
                if (buffer.size() == 0)
                {
                    return std::nullopt;
                }
                return std::make_pair(buffer, false);
            }

            auto chunk = inner_->next(ec);
            if (ec)
            {
                return std::nullopt;
            }
            if (!chunk)
            {
                inner_done_ = true;
                continue;
            }

            auto const more = chunk->second;
            encoder_.feed(chunk->first, more, ec);
            if (ec)
            {
                return std::nullopt;
            }
            if (!more)
            {
                // 内层最后一块已喂入（feed 内部会 close 编码流），无需再 flush。
                inner_done_ = true;
                flushed_ = true;
            }

            auto buffer = encoder_.buffer();
            if (buffer.size() != 0)
            {
                return std::make_pair(buffer, !more);
            }
        }
    }

    // ---- form_data_source ----

    form_data_source::form_data_source(httplib::form_data const& body) : body_(&body) {}

    source::chunk_t
    form_data_source::next(boost::system::error_code& ec)
    {
        ec = {};
        if (field_index_ >= body_->fields.size())
        {
            return std::nullopt;
        }
        buffer_.consume(buffer_.size());

        auto& field_data = body_->fields[field_index_];
        switch (step_)
        {
            case step::header:
            {
                std::string header = fmt::format("--{}\r\n", body_->boundary);
                header += fmt::format(R"(Content-Disposition: form-data; name="{}")", field_data.name);
                if (!field_data.filename.empty())
                {
                    header += fmt::format(R"(; filename="{}")", field_data.filename);
                }
                header += "\r\n";
                if (!field_data.content_type.empty())
                {
                    header += fmt::format("Content-Type: {}\r\n", field_data.content_type);
                }
                header += "\r\n";
                net::buffer_copy(buffer_.prepare(header.size()), net::buffer(header));
                buffer_.commit(header.size());

                step_ = step::content;
                return std::make_pair(buffer_.cdata(), true);
            }
            case step::content:
            {
                if (field_data.file_path)
                {
                    if (!file_stream_.is_open())
                    {
                        file_stream_.open(*field_data.file_path, std::ios::in | std::ios::binary);
                        if (!file_stream_.is_open())
                        {
                            ec = boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory);
                            return std::nullopt;
                        }
                        std::error_code fs_ec;
                        file_remaining_ = fs::file_size(*field_data.file_path, fs_ec);
                        if (fs_ec)
                        {
                            file_stream_.close();
                            ec = fs_ec;
                            return std::nullopt;
                        }
                    }
                    if (file_remaining_ == 0)
                    {
                        file_stream_.close();
                        step_ = step::content_end;
                        return next(ec);
                    }
                    auto n = (std::min)(static_cast<std::uintmax_t>(file_buf_size_), file_remaining_);
                    file_stream_.read(file_buf_.data(), static_cast<std::streamsize>(n));
                    auto read = static_cast<std::uintmax_t>(file_stream_.gcount());
                    file_remaining_ -= read;
                    if (file_remaining_ == 0)
                    {
                        file_stream_.close();
                        step_ = step::content_end;
                    }
                    return std::make_pair(net::const_buffer(file_buf_.data(), read), true);
                }
                step_ = step::content_end;
                return std::make_pair(net::const_buffer(field_data.content.data(), field_data.content.size()), true);
            }
            case step::content_end:
            {
                bool is_eof = field_index_ == body_->fields.size() - 1;
                std::string end("\r\n");
                if (is_eof)
                {
                    end += fmt::format("--{}--\r\n", body_->boundary);
                    step_ = step::eof;
                }
                else
                {
                    step_ = step::header;
                    field_index_++;
                }
                net::buffer_copy(buffer_.prepare(end.size()), net::buffer(end));
                buffer_.commit(end.size());
                return std::make_pair(buffer_.cdata(), !is_eof);
            }
            default:
                break;
        }
        return std::nullopt;
    }

    // ---- file_source ----

    file_source::file_source(fs::path path, html::http_ranges ranges, std::string content_type, std::string boundary)
        : path_(std::move(path))
        , ranges_(std::move(ranges))
        , content_type_(std::move(content_type))
        , boundary_(std::move(boundary))
    {
        file_.open(path_, std::ios::in | std::ios::binary);
        if (!file_.is_open())
        {
            open_ec_ = boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory);
            return;
        }
        file_.seekg(0, std::ios::end);
        file_size_ = static_cast<std::size_t>(file_.tellg());
        file_.seekg(0, std::ios::beg);
    }

    std::size_t
    file_source::read(char* dest, std::size_t n)
    {
        file_.read(dest, static_cast<std::streamsize>(n));
        return static_cast<std::size_t>(file_.gcount());
    }

    source::chunk_t
    file_source::next(boost::system::error_code& ec)
    {
        ec = {};
        if (open_ec_)
        {
            ec = open_ec_;
            return std::nullopt;
        }

        if (ranges_.size() == 1 || ranges_.empty())
        {
            html::http_ranges::range_type range;
            if (ranges_.empty())
            {
                range = { 0, static_cast<std::int64_t>(file_size_) };
            }
            else
            {
                range = ranges_.ranges().front();
                range.second = range.second + 1;
            }

            if (!pos_)
            {
                pos_ = static_cast<std::uint64_t>(range.first);
                file_.seekg(range.first);
            }
            std::size_t const n = (std::min)(
                sizeof(buf_),
                static_cast<std::size_t>(beast::detail::clamp(range.second - static_cast<std::int64_t>(*pos_))));
            if (n == 0)
            {
                ec = {};
                return std::nullopt;
            }
            auto const nread = read(buf_, n);
            if (nread == 0)
            {
                ec = http::error::short_read;
                return std::nullopt;
            }
            *pos_ += nread;
            return std::make_pair(net::const_buffer(buf_, nread), *pos_ < static_cast<std::uint64_t>(range.second));
        }

        if (!range_index_)
        {
            range_index_ = 0;
            step_ = step::header;
        }

        if (*range_index_ >= static_cast<int>(ranges_.size()))
        {
            ec = {};
            return std::nullopt;
        }
        auto const& range = ranges_.at(static_cast<std::size_t>(*range_index_));
        switch (step_)
        {
            case step::header:
            {
                std::string header = fmt::format("--{}\r\n", boundary_);
                header += fmt::format("Content-Type: {}\r\n", content_type_);
                header += fmt::format("Content-Range: bytes {}-{}/{}\r\n", range.first, range.second, file_size_);
                header += "\r\n";
                net::buffer_copy(net::buffer(buf_, sizeof(buf_)), net::buffer(header));
                step_ = step::content;
                pos_ = std::nullopt;
                return std::make_pair(net::const_buffer(buf_, header.size()), true);
            }
            case step::content:
            {
                if (!pos_)
                {
                    pos_ = static_cast<std::uint64_t>(range.first);
                    file_.seekg(range.first);
                }
                std::size_t const n = (std::min)(
                    sizeof(buf_),
                    static_cast<std::size_t>(beast::detail::clamp(range.second - static_cast<std::int64_t>(*pos_))));
                if (n == 0)
                {
                    step_ = step::content_end;
                    return next(ec);
                }
                auto const nread = read(buf_, n);
                if (nread == 0)
                {
                    ec = http::error::short_read;
                    return std::nullopt;
                }
                *pos_ += nread;
                return std::make_pair(net::const_buffer(buf_, nread), true);
            }
            case step::content_end:
            {
                bool is_eof = (*range_index_) == static_cast<int>(ranges_.size()) - 1;
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
                return std::make_pair(net::const_buffer(buf_, end.size()), !is_eof);
            }
            default:
                break;
        }
        return std::nullopt;
    }

} // namespace httplib::body
