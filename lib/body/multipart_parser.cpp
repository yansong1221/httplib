#include "body/multipart_parser.hpp"
#include "html/html.h"
#include "httplib/util/misc.hpp"
#include <algorithm>
#include <boost/algorithm/string/trim.hpp>
#include <boost/beast/http/error.hpp>
#include <cctype>
#include <cstddef>
#include <utility>

namespace httplib::body
{
    using namespace std::string_view_literals;

    namespace
    {
        // 返回 sv 的最长后缀，且该后缀是 a 或 b 的前缀。用于把可能被切开的
        // multipart boundary 分隔符保留在 pending 中，而不是当成正文提交。
        std::size_t
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

        auto
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

        auto
        split_header_field_value(std::string_view header, boost::system::error_code& ec)
        {
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
                    ec = http::error::unexpected_body;
                    return decltype(results) {};
                }

                auto key = boost::trim_copy(line.substr(0, pos));
                auto value = boost::trim_copy(line.substr(pos + 1));
                results.emplace_back(key, value);
            }

            return results;
        }
    } // namespace

    multipart_parser::multipart_parser(std::string content_type, httplib::form_data::param params)
        : content_type_(std::move(content_type))
    {
        body_.params = std::move(params);
    }

    void
    multipart_parser::reset(boost::system::error_code& ec)
    {
        ec = {};

        auto content_type_parts = util::split(content_type_, ";"sv);

        for (auto const& part : content_type_parts)
        {
            auto trimmed_part = boost::trim_copy(part);
            if (!trimmed_part.starts_with("boundary"))
            {
                continue;
            }

            auto const& boundary_pair = util::split(trimmed_part, "="sv);
            if (boundary_pair.size() != 2)
            {
                continue;
            }

            boundary_ = boost::trim_copy(boundary_pair[1]);
        }
        if (boundary_.empty())
        {
            ec = http::error::bad_field;
        }
        else
        {
            body_.boundary = boundary_;
            boundary_line_ = "--" + boundary_ + "\r\n";
            boundary_line_last_ = "--" + boundary_ + "--\r\n";
            delim_field_ = "\r\n--" + boundary_ + "\r\n";
            delim_final_ = "\r\n--" + boundary_ + "--\r\n";
            pending_.clear();
            combined_.clear();
        }
    }

    void
    multipart_parser::put(net::const_buffer const& buffers, boost::system::error_code& ec)
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
            return;
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
                    auto results = split_header_field_value(header, ec);
                    if (ec)
                    {
                        break;
                    }

                    httplib::form_data::field field_data;
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

                            auto result = parse_content_disposition(value);
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

                    bool save_to_file = !field_data_.filename.empty() && !body_.params.save_dir.empty();
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
                        if (body_.params.max_fields != 0 && body_.fields.size() >= body_.params.max_fields)
                        {
                            ec = http::error::body_limit;
                            break;
                        }
                        body_.fields.push_back(std::move(field_data_));
                        step_ = is_final ? step::eof : step::boundary_header;
                        continue;
                    }

                    auto keep = longest_suffix_prefix(sv, delim_field_, delim_final_);
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
            return;
        }
        if (need_more_data)
        {
            pending_.assign(sv);
        }
    }

    void
    multipart_parser::finish(boost::system::error_code& ec)
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
    multipart_parser::write_content(std::string_view data, boost::system::error_code& ec)
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
            auto candidate = body_.params.save_dir / safe_name;
            auto canonical_dir = fs::weakly_canonical(body_.params.save_dir);
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
        if (body_.params.max_file_size)
        {
            file_bytes_written_ += data.size();
            if (file_bytes_written_ > body_.params.max_file_size)
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
