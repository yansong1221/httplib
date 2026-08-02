#include "httplib/body/form_data_body.hpp"
#include "html/html.h"
#include "httplib/util/misc.hpp"
#include <fmt/format.h>
#include <random>
#include <string_view>

namespace httplib::body
{
    using namespace std::string_view_literals;

    namespace detail
    {
        static auto
        parse_content_disposition(std::string_view header)
        {
            std::vector<std::pair<std::string_view, std::string_view>> results;

            size_t pos = 0;
            while (pos < header.size())
            {
                size_t eq = header.find('=', pos);
                if (eq == std::string_view::npos)
                {
                    break;
                }

                std::string_view key = header.substr(pos, eq - pos);
                key = boost::trim_copy(key);
                pos = eq + 1;

                std::string_view value;
                if (pos < header.size() && header[pos] == '"')
                {
                    pos++;
                    size_t end = pos;
                    bool escape = false;
                    while (end < header.size())
                    {
                        if (header[end] == '\\' && !escape)
                        {
                            escape = true;
                        }
                        else if (header[end] == '"' && !escape)
                        {
                            break;
                        }
                        else
                        {
                            escape = false;
                        }
                        end++;
                    }
                    value = header.substr(pos, end - pos);
                    pos = (end < header.size()) ? end + 1 : end;
                }
                else
                {
                    size_t end = header.find(';', pos);
                    if (end == std::string_view::npos)
                    {
                        end = header.size();
                    }
                    value = header.substr(pos, end - pos);
                    value = boost::trim_copy(value);
                    pos = end;
                }

                results.emplace_back(key, value);

                if (pos < header.size() && header[pos] == ';')
                {
                    pos++;
                }
                while (pos < header.size() && std::isspace(header[pos]))
                {
                    pos++;
                }
            }
            return results;
        }
        static auto
        split_header_field_value(std::string_view header, boost::system::error_code& ec)
        {
            using namespace std::string_view_literals;

            std::vector<std::pair<std::string_view, std::string_view>> results;
            auto lines = util::split(header, "\r\n"sv);

            for (auto const& line : lines)
            {
                if (line.empty())
                {
                    continue;
                }

                auto pos = line.find(":");
                if (pos == std::string_view::npos)
                {
                    ec = boost::beast::http::error::unexpected_body;
                    return decltype(results) {};
                }

                auto key = boost::trim_copy(line.substr(0, pos));
                auto value = boost::trim_copy(line.substr(pos + 1));
                results.emplace_back(key, value);
            }

            return results;
        }

    } // namespace detail

    form_data_body::writer::writer(http::fields const&, value_type& b) : body_(b) {}

    boost::optional<std::pair<form_data_body::writer::const_buffers_type, bool>>
    form_data_body::writer::get(boost::system::error_code& ec)
    {
        ec = {};
        if (field_data_index_ >= body_.fields.size())
        {
            return boost::none;
        }
        buffer_.consume(buffer_.size());

        auto& field_data = body_.fields[field_data_index_];
        switch (step_)
        {
            case step::header:
            {
                std::string header = fmt::format("--{}\r\n", body_.boundary);

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
            break;
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
                            return boost::none;
                        }
                        std::error_code fs_ec;
                        file_remaining_ = fs::file_size(*field_data.file_path, fs_ec);
                        if (fs_ec)
                        {
                            file_stream_.close();
                            ec = fs_ec;
                            return boost::none;
                        }
                    }
                    if (file_remaining_ == 0)
                    {
                        file_stream_.close();
                        step_ = step::content_end;
                        return get(ec);
                    }
                    auto n = std::min<std::uintmax_t>(file_buf_size_, file_remaining_);
                    file_stream_.read(file_buf_.data(), static_cast<std::streamsize>(n));
                    auto read = static_cast<std::uintmax_t>(file_stream_.gcount());
                    file_remaining_ -= read;
                    if (file_remaining_ == 0)
                    {
                        file_stream_.close();
                        step_ = step::content_end;
                    }
                    return std::make_pair<const_buffers_type>(net::buffer(file_buf_.data(), read), true);
                }
                step_ = step::content_end;
                return std::make_pair<const_buffers_type>(net::buffer(field_data.content), true);
            }
            break;
            case step::content_end:
            {
                bool is_eof = field_data_index_ == body_.fields.size() - 1;
                std::string end("\r\n");
                if (is_eof)
                {
                    end += fmt::format("--{}--\r\n", body_.boundary);
                    step_ = step::eof;
                }
                else
                {
                    step_ = step::header;
                    field_data_index_++;
                }
                net::buffer_copy(buffer_.prepare(end.size()), net::buffer(end));
                buffer_.commit(end.size());
                return std::make_pair(buffer_.cdata(), !is_eof);
            }
            break;
            default:
                break;
        }
        return boost::none;
    }

    void
    form_data_body::writer::init(boost::system::error_code& ec)
    {
        ec.clear();
        field_data_index_ = 0;
        file_stream_.close();
        file_remaining_ = 0;
    }

    form_data_body::reader::reader(http::fields const& h, value_type& b) : body_(b)
    {
        content_type_ = h[http::field::content_type];
    }

    void
    form_data_body::reader::init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
    {
        boost::ignore_unused(content_length);
        ec = {};

        auto content_type_parts = util::split(content_type_, ";"sv);

        // Look for boundary
        for (auto const& part : content_type_parts)
        {
            auto trimed_part = boost::trim_copy(part);
            // Look for part containing boundary
            if (!trimed_part.starts_with("boundary"))
            {
                continue;
            }

            // Extract boundary
            auto const& boundary_pair = util::split(trimed_part, "="sv);
            if (boundary_pair.size() != 2)
            {
                continue;
            }

            // Assign
            boundary_ = boost::trim_copy(boundary_pair[1]);
        }
        if (boundary_.empty())
        {
            ec = http::error::bad_field;
        }
    }
    std::size_t
    form_data_body::reader::put(const_buffers_type const& buffers, boost::system::error_code& ec)
    {
        switch (step_)
        {
            case step::boundary_line:
            {
                std::string const boundary_line = "--" + boundary_ + "\r\n";
                std::string const boundary_line_last = "--" + boundary_ + "--";

                if (beast::buffer_bytes(buffers) < std::max(boundary_line.size(), boundary_line_last.size()))
                {
                    ec = http::error::need_more;
                    return 0;
                }
                auto data = util::buffer_to_string_view(buffers);

                if (data.starts_with(boundary_line))
                {
                    step_ = step::boundary_header;
                    return boundary_line.size();
                }
                else if (data.starts_with(boundary_line_last))
                {
                    step_ = step::finished;
                    return boundary_line_last.size();
                }
                ec = http::error::unexpected_body;
                return 0;
            }
            break;
            case step::boundary_header:
            {
                auto data = util::buffer_to_string_view(buffers);
                auto pos = data.find("\r\n\r\n");
                if (pos == std::string_view::npos)
                {
                    ec = http::error::need_more;
                    return 0;
                }
                auto header = data.substr(0, pos + 4);
                auto results = detail::split_header_field_value(header, ec);
                if (ec)
                {
                    return 0;
                }

                html::form_data::field field_data;
                for (auto const& item : results)
                {
                    if (item.first == "Content-Disposition"sv)
                    {
                        auto value = item.second;

                        auto pos = value.find(";");
                        if (pos == std::string_view::npos)
                        {
                            ec = http::error::unexpected_body;
                            return 0;
                        }
                        else if (boost::trim_copy(value.substr(0, pos)) != "form-data")
                        {
                            ec = http::error::unexpected_body;
                            return 0;
                        }
                        value.remove_prefix(pos + 1);

                        auto result = detail::parse_content_disposition(value);
                        for (auto const& pair : result)
                        {
                            if (pair.first == "name")
                            {
                                field_data.name = pair.second;
                            }
                            else if (pair.first == "filename")
                            {
                                field_data.filename = pair.second;
                            }
                        }
                    }
                    else if (item.first == "Content-Type"sv)
                    {
                        field_data.content_type = item.second;
                    }
                }

                field_data_ = std::move(field_data);
                step_ = step::boundary_content;
                return header.length();
            }
            break;
            case step::boundary_content:
            {
                auto data = util::buffer_to_string_view(buffers);
                bool save_to_file = !field_data_.filename.empty() && !body_.save_dir.empty();
                if (data.starts_with("\r"))
                {
                    std::string const eof_boundary_line = "\r\n--" + boundary_;

                    if (beast::buffer_bytes(buffers) < eof_boundary_line.size())
                    {
                        ec = http::error::need_more;
                        return 0;
                    }
                    if (data.starts_with(eof_boundary_line))
                    {
                        if (save_to_file)
                        {
                            if (file_stream_.is_open())
                            {
                                file_stream_.close();
                            }
                            field_data_.file_path = current_file_path_;
                        }
                        step_ = step::boundary_line;
                        body_.fields.push_back(std::move(field_data_));
                        return 2;
                    }
                    if (save_to_file)
                    {
                        write_content("\r", ec);
                        if (ec)
                        {
                            return 0;
                        }
                    }
                    else
                    {
                        field_data_.content.push_back('\r');
                    }
                    return 1;
                }
                auto pos = data.find("\r");
                if (pos == std::string_view::npos)
                {
                    if (save_to_file)
                    {
                        write_content(data, ec);
                        if (ec)
                        {
                            return 0;
                        }
                    }
                    else
                    {
                        field_data_.content.append(data);
                    }
                    return data.length();
                }
                if (save_to_file)
                {
                    write_content(data.substr(0, pos), ec);
                    if (ec)
                    {
                        return 0;
                    }
                }
                else
                {
                    field_data_.content.append(data.substr(0, pos));
                }
                return pos;
            }
            break;
            case step::finished:
            {
                if (beast::buffer_bytes(buffers) < 2)
                {
                    ec = http::error::need_more;
                    return 0;
                }
                auto data = util::buffer_to_string_view(buffers);
                if (!data.starts_with("\r\n"))
                {
                    ec = http::error::unexpected_body;
                    return 0;
                }
                step_ = step::eof;
                return 2;
            }
            break;
            default:
                break;
        }

        ec = http::error::unexpected_body;
        return 0;
    }

    void
    form_data_body::reader::finish(boost::system::error_code& ec)
    {
        ec.clear();
        if (file_stream_.is_open())
        {
            file_stream_.close();
        }
        if (step_ != step::eof)
        {
            ec = http::error::partial_message;
        }
    }

    void
    form_data_body::reader::write_content(std::string_view data, boost::system::error_code& ec)
    {
        if (!file_stream_.is_open())
        {
            auto name = field_data_.filename.empty() ? "upload" : field_data_.filename;
            current_file_path_ = body_.save_dir / name;
            file_bytes_written_ = 0;
            file_stream_.open(current_file_path_, std::ios::out | std::ios::binary | std::ios::trunc);
        }
        if (body_.max_file_size)
        {
            file_bytes_written_ += data.size();
            if (file_bytes_written_ > body_.max_file_size)
            {
                file_stream_.close();
                std::error_code rm_ec;
                fs::remove(current_file_path_, rm_ec);
                ec = http::error::body_limit;
                return;
            }
        }
        file_stream_.write(data.data(), static_cast<std::streamsize>(data.size()));
    }

} // namespace httplib::body