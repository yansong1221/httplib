#pragma once
#include "../utils.hpp"
#include "form_data.hpp"
#include <boost/algorithm/string/trim.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http/message.hpp>

namespace httplib {

namespace beast = boost::beast;   // from <boost/beast.hpp>
namespace http = beast::http;     // from <boost/beast/http.hpp>
namespace net = boost::asio;      // from <boost/asio.hpp>
namespace ssl = boost::asio::ssl; // from <boost/asio/ssl.hpp>
using tcp = boost::asio::ip::tcp; // from <boost/asio/ip/tcp.hpp>
namespace websocket = beast::websocket;

using namespace std::string_view_literals;

class form_data_body {
public:
    using value_type = httplib::form_data;

    class writer {
    public:
        using const_buffers_type = net::const_buffer;

        template<bool isRequest, class Fields>
        writer(http::header<isRequest, Fields> &h, value_type &b)
            : body_(b), boundary_(generate_boundary()) {
            h.set(http::field::content_type,
                  std::format("multipart/form-data; boundary={}", boundary_));
        }

        void init(boost::system::error_code &ec) {
            ec.clear();
            boundary_ = generate_boundary();
            field_data_index_ = 0;
        }

        boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code &ec) {

            if (field_data_index_ >= body_.fields.size()) {
                return boost::none;
            }
            buffer_.consume(buffer_.size());

            auto &field_data = body_.fields[field_data_index_];
            switch (step_) {
            case step::header: {
                std::string header = std::format("--{}\r\n", boundary_);

                header +=
                    std::format(R"(Content-Disposition: form-data; name="{}")", field_data.name);
                if (!field_data.filename.empty()) {
                    header += std::format(R"(; filename="{}")", field_data.filename);
                }
                header += "\r\n";
                if (!field_data.content_type.empty()) {
                    header += std::format("Content-Type: {}\r\n", field_data.content_type);
                }
                header += "\r\n";
                net::buffer_copy(buffer_.prepare(header.size()), net::buffer(header));
                buffer_.commit(header.size());

                step_ = step::content;
                return std::make_pair(buffer_.cdata(), true);

            } break;
            case step::content: {
                step_ = step::content_end;
                return std::make_pair<const_buffers_type>(net::buffer(field_data.content), true);
            } break;
            case step::content_end: {

                bool is_eof = field_data_index_ == body_.fields.size() - 1;
                std::string end("\r\n");
                if (is_eof) {
                    end += std::format("--{}--\r\n", boundary_);
                    step_ = step::eof;
                } else {
                    step_ = step::header;
                    field_data_index_++;
                }
                net::buffer_copy(buffer_.prepare(end.size()), net::buffer(end));
                buffer_.commit(end.size());
                return std::make_pair(buffer_.cdata(), !is_eof);
            } break;
            default:
                break;
            }
            return boost::none;
        }

    private:
        static std::string generate_boundary() {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<int> dist(100000, 999999);

            return "----------------" + std::to_string(millis) + std::to_string(dist(gen));
        }

    private:
        value_type &body_;
        std::string boundary_;
        int field_data_index_ = 0;
        beast::flat_buffer buffer_;

        enum class step {
            header,
            content,
            content_end,
            eof
        };
        step step_ = step::header;
    };

    //--------------------------------------------------------------------------

    class reader {

    public:
        template<bool isRequest, class Fields>
        explicit reader(http::header<isRequest, Fields> &h, value_type &b) : body_(b) {
            content_type_ = h[http::field::content_type];
        }

        void init(boost::optional<std::uint64_t> const &content_length,
                  boost::system::error_code &ec) {

            // VFALCO We could reserve space in the file
            boost::ignore_unused(content_length);
            ec = {};

            auto content_type_parts = utils::split(content_type_, ";"sv);

            // Look for boundary
            for (const auto &part : content_type_parts) {
                auto trimed_part = boost::trim_copy(part);
                // Look for part containing boundary
                if (!trimed_part.starts_with("boundary"))
                    continue;

                // Extract boundary
                const auto &boundary_pair = utils::split(trimed_part, "="sv);
                if (boundary_pair.size() != 2)
                    continue;

                // Assign
                boundary_ = boost::trim_copy(boundary_pair[1]);
            }
            if (boundary_.empty()) {
                ec = http::error::bad_field;
            }
        }

        template<class ConstBufferSequence>
        std::size_t put(ConstBufferSequence const &buffers, boost::system::error_code &ec) {

            switch (step_) {
            case step::boundary_line: {
                const std::string boundary_line = "--" + boundary_ + "\r\n";
                const std::string boundary_line_last = "--" + boundary_ + "--";

                if (beast::buffer_bytes(buffers) <
                    std::max(boundary_line.size(), boundary_line_last.size())) {
                    ec = http::error::need_more;
                    return 0;
                }
                auto data = utils::buffer_to_string_view(buffers);

                if (data.starts_with(boundary_line)) {
                    step_ = step::boundary_header;
                    return boundary_line.size();
                } else if (data.starts_with(boundary_line_last)) {
                    step_ = step::finshed;
                    return boundary_line_last.size();
                }
                ec = http::error::unexpected_body;
                return 0;

            } break;
            case step::boundary_header: {

                auto data = utils::buffer_to_string_view(buffers);
                auto pos = data.find("\r\n\r\n");
                if (pos == std::string_view::npos) {
                    ec = http::error::need_more;
                    return 0;
                }
                auto header = data.substr(0, pos + 4);
                auto results = split_header_field_value(header, ec);
                if (ec)
                    return 0;

                form_field_data field_data;
                for (const auto &item : results) {
                    if (item.first == key_content_disposition) {

                        auto value = item.second;

                        auto pos = value.find(";");
                        if (pos == std::string_view::npos) {
                            ec = http::error::unexpected_body;
                            return 0;
                        } else if (boost::trim_copy(value.substr(0, pos)) != "form-data") {
                            ec = http::error::unexpected_body;
                            return 0;
                        }
                        value.remove_prefix(pos + 1);

                        auto result = parse_content_disposition(value);
                        for (const auto pair : result) {
                            if (pair.first == "name") {
                                field_data.name = pair.second;

                            } else if (pair.first == "filename") {
                                field_data.filename = pair.second;
                            }
                        }
                    } else if (item.first == key_content_type) {
                        field_data.content_type = item.second;
                    }
                }

                field_data_ = std::move(field_data);
                step_ = step::boundary_content;
                return header.length();
            } break;
            case step::boundary_content: {

                auto data = utils::buffer_to_string_view(buffers);
                if (data.starts_with("\r")) {

                    const std::string eof_boundary_line = "\r\n--" + boundary_;

                    if (beast::buffer_bytes(buffers) < eof_boundary_line.size()) {
                        ec = http::error::need_more;
                        return 0;
                    }
                    if (data.starts_with(eof_boundary_line)) {
                        step_ = step::boundary_line;
                        body_.fields.push_back(std::move(field_data_));
                        return 2;
                    }
                    field_data_.content.push_back('\r');
                    return 1;
                }
                auto pos = data.find("\r");
                if (pos == std::string_view::npos) {
                    field_data_.content.append(data);
                    return data.length();
                }
                field_data_.content.append(data.substr(0, pos));
                return pos;

            } break;
            case step::finshed: {
                if (beast::buffer_bytes(buffers) < 2) {
                    ec = http::error::need_more;
                    return 0;
                }
                auto data = utils::buffer_to_string_view(buffers);
                if (!data.starts_with("\r\n")) {
                    ec = http::error::unexpected_body;
                    return 0;
                }
                step_ = step::eof;
                return 2;
            } break;
            default:
                break;
            }

            ec = http::error::unexpected_body;
            return 0;
        }

        void finish(boost::system::error_code &ec) {
            ec.clear();
            if (step_ != step::eof) {
                ec = http::error::partial_message;
            }
        }

    private:
        static auto parse_content_disposition(std::string_view header) {
            std::vector<std::pair<std::string_view, std::string_view>> results;

            size_t pos = 0;
            while (pos < header.size()) {
                size_t eq = header.find('=', pos);
                if (eq == std::string_view::npos)
                    break;

                std::string_view key = header.substr(pos, eq - pos);
                key = boost::trim_copy(key); // 去掉 key 的前后空格
                pos = eq + 1;

                std::string_view value;
                if (pos < header.size() && header[pos] == '"') { // 处理双引号值
                    pos++;
                    size_t end = pos;
                    bool escape = false;
                    while (end < header.size()) {
                        if (header[end] == '\\' && !escape) {
                            escape = true;
                        } else if (header[end] == '"' && !escape) {
                            break;
                        } else {
                            escape = false;
                        }
                        end++;
                    }
                    value = header.substr(pos, end - pos);
                    pos = (end < header.size()) ? end + 1 : end; // 跳过 `"`
                } else {                                         // 处理非双引号值
                    size_t end = header.find(';', pos);
                    if (end == std::string_view::npos)
                        end = header.size();
                    value = header.substr(pos, end - pos);
                    value = boost::trim_copy(value); // 去掉 value 的前后空格
                    pos = end;
                }

                results.emplace_back(key, value);

                // 处理 `; ` 分隔符
                if (pos < header.size() && header[pos] == ';')
                    pos++;
                while (pos < header.size() && std::isspace(header[pos])) // 跳过空格
                    pos++;
            }
            return results;
        }
        static auto split_header_field_value(std::string_view header,
                                             boost::system::error_code &ec) {
            std::vector<std::pair<std::string_view, std::string_view>> results;
            auto lines = utils::split(header, "\r\n"sv);

            for (const auto &line : lines) {
                if (line.empty())
                    continue;

                auto pos = line.find(":");
                if (pos == std::string_view::npos) {
                    ec = http::error::unexpected_body;
                    return decltype(results){};
                }

                auto key = boost::trim_copy(line.substr(0, pos));
                auto value = boost::trim_copy(line.substr(pos + 1));
                results.emplace_back(key, value);
            }

            return results;
        }

    private:
        constexpr static std::string_view key_content_disposition = "Content-Disposition"sv;
        constexpr static std::string_view key_content_type = "Content-Type"sv;

        value_type &body_;
        std::string_view content_type_;
        std::string boundary_;
        enum class step {
            boundary_line,
            boundary_header,
            boundary_content,
            finshed,
            eof
        };
        step step_ = step::boundary_line;
        form_field_data field_data_;
    };
};
} // namespace httplib