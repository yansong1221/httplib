#include "body/form_data_body.hpp"
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
        // Returns the length of the longest suffix of `sv` that is a prefix of
        // `a` or `b`. Used to keep a possible half-split multipart boundary
        // delimiter in the reader's pending buffer instead of committing it as
        // part content.
        static std::size_t
        longest_suffix_prefix(std::string_view sv, std::string_view a, std::string_view b)
        {
            auto const max_len = (std::max)(a.size(), b.size());
            auto const m = (std::min)(sv.size(), max_len);
            for (std::size_t k = m; k >= 1; --k)
            {
                auto const tail = sv.substr(sv.size() - k);
                if (a.size() >= k && a.substr(0, k) == tail)
                {
                    return k;
                }
                if (b.size() >= k && b.substr(0, k) == tail)
                {
                    return k;
                }
            }
            return 0;
        }
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
                while (pos < header.size() && std::isspace(static_cast<unsigned char>(header[pos])))
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
        else
        {
            boundary_line_ = "--" + boundary_ + "\r\n";
            boundary_line_last_ = "--" + boundary_ + "--\r\n";
            delim_field_ = "\r\n--" + boundary_ + "\r\n";
            delim_final_ = "\r\n--" + boundary_ + "--\r\n";
            pending_.clear();
            combined_.clear();
        }
    }
    std::size_t
    form_data_body::reader::put(const_buffers_type const& buffers, boost::system::error_code& ec)
    {
        ec = {};
        auto incoming = util::buffer_to_string_view(buffers);

        std::string_view sv;
        if (pending_.empty())
        {
            sv = incoming;
        }
        else
        {
            combined_.clear();
            combined_.reserve(pending_.size() + incoming.size());
            combined_.append(pending_);
            combined_.append(incoming.data(), incoming.size());
            pending_.clear();
            sv = combined_;
        }

        bool need_more_data = false;

        if (step_ == step::eof)
        {
            if (!sv.empty())
            {
                ec = http::error::unexpected_body;
            }
            return incoming.size();
        }

        for (;;)
        {
            if (sv.empty())
            {
                break;
            }
            switch (step_)
            {
                case step::boundary_line:
                {
                    if (sv.size() >= boundary_line_.size() && sv.substr(0, boundary_line_.size()) == boundary_line_)
                    {
                        sv.remove_prefix(boundary_line_.size());
                        step_ = step::boundary_header;
                        continue;
                    }
                    if (sv.size() >= boundary_line_last_.size()
                        && sv.substr(0, boundary_line_last_.size()) == boundary_line_last_)
                    {
                        sv.remove_prefix(boundary_line_last_.size());
                        step_ = step::eof;
                        continue;
                    }
                    if (boundary_line_.starts_with(sv) || boundary_line_last_.starts_with(sv))
                    {
                        need_more_data = true;
                        break;
                    }
                    ec = http::error::unexpected_body;
                }
                break;
                case step::boundary_header:
                {
                    auto pos = sv.find("\r\n\r\n");
                    if (pos == std::string_view::npos)
                    {
                        need_more_data = true;
                        break;
                    }
                    auto header = sv.substr(0, pos + 4);
                    auto results = detail::split_header_field_value(header, ec);
                    if (ec)
                    {
                        break;
                    }

                    html::form_data::field field_data;
                    for (auto const& item : results)
                    {
                        if (item.first == "Content-Disposition"sv)
                        {
                            auto value = item.second;

                            auto semi = value.find(";");
                            if (semi == std::string_view::npos)
                            {
                                ec = http::error::unexpected_body;
                                break;
                            }
                            else if (boost::trim_copy(value.substr(0, semi)) != "form-data")
                            {
                                ec = http::error::unexpected_body;
                                break;
                            }
                            value.remove_prefix(semi + 1);

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
                    if (ec)
                    {
                        break;
                    }

                    field_data_ = std::move(field_data);
                    sv.remove_prefix(pos + 4);
                    step_ = step::boundary_content;
                    continue;
                }
                break;
                case step::boundary_content:
                {
                    auto pos_field = sv.find(delim_field_);
                    auto pos_final = sv.find(delim_final_);
                    std::size_t pos = std::string_view::npos;
                    bool is_final = false;
                    if (pos_field != std::string_view::npos)
                    {
                        if (pos_final == std::string_view::npos || pos_field <= pos_final)
                        {
                            pos = pos_field;
                        }
                        else
                        {
                            pos = pos_final;
                            is_final = true;
                        }
                    }
                    else if (pos_final != std::string_view::npos)
                    {
                        pos = pos_final;
                        is_final = true;
                    }

                    bool save_to_file = !field_data_.filename.empty() && !body_.save_dir.empty();
                    auto commit_content = [&](std::string_view data)
                    {
                        if (save_to_file)
                        {
                            write_content(data, ec);
                        }
                        else
                        {
                            field_data_.content.append(data.data(), data.size());
                        }
                    };

                    if (pos != std::string_view::npos)
                    {
                        if (pos > 0)
                        {
                            commit_content(sv.substr(0, pos));
                            if (ec)
                            {
                                break;
                            }
                        }
                        if (save_to_file && file_stream_.is_open())
                        {
                            file_stream_.close();
                            field_data_.file_path = current_file_path_;
                        }
                        auto const delim_size = is_final ? delim_final_.size() : delim_field_.size();
                        sv.remove_prefix(pos + delim_size);
                        body_.fields.push_back(std::move(field_data_));
                        step_ = is_final ? step::eof : step::boundary_header;
                        continue;
                    }

                    auto keep = detail::longest_suffix_prefix(sv, delim_field_, delim_final_);
                    auto emit_size = sv.size() - keep;
                    if (emit_size > 0)
                    {
                        commit_content(sv.substr(0, emit_size));
                        if (ec)
                        {
                            break;
                        }
                    }
                    sv.remove_prefix(emit_size);
                    if (keep > 0)
                    {
                        need_more_data = true;
                    }
                }
                break;
                default:
                    break;
            }
            break;
        }

        if (step_ == step::eof && !sv.empty())
        {
            ec = http::error::unexpected_body;
        }
        if (ec)
        {
            return incoming.size();
        }
        if (need_more_data)
        {
            pending_.assign(sv);
            ec = http::error::need_more;
        }
        return incoming.size();
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
            auto safe_name = field_data_.filename.empty()
                                 ? "upload"
                                 : std::string(fs::path(field_data_.filename).filename().string());
            if (safe_name.empty() || safe_name == "." || safe_name == "..")
            {
                safe_name = "upload";
            }
            auto candidate = body_.save_dir / safe_name;
            auto canonical_dir = fs::weakly_canonical(body_.save_dir);
            auto canonical_file = fs::weakly_canonical(candidate);
            if (canonical_file.string().rfind(canonical_dir.string(), 0) != 0)
            {
                ec = http::error::body_limit;
                return;
            }
            current_file_path_ = candidate;
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