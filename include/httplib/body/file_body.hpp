#pragma once
#include "httplib/config.hpp"
#include "httplib/html.hpp"
#include "httplib/util/misc.hpp"
#include <boost/beast/core/file.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/message.hpp>
#include <fmt/format.h>
#include <fstream>

namespace httplib::body {
struct file_body
{
    struct value_type
    {
        html::http_ranges ranges;
        std::string content_type;
        std::string boundary;

        std::size_t file_size() const { return file_size_; }

        void seekg(std::ios::off_type _Off, std::ios_base::seekdir _Way = std::ios::cur)
        {
            file_.seekg(_Off, _Way);
        }
        std::size_t read(void* buffer, std::size_t n)
        {
            file_.read((char*)buffer, n);
            return file_.gcount();
        }
        void open(const fs::path& path,
                  std::ios_base::openmode mode = std::ios_base::in | std::ios_base::out)
        {
            file_size_ = 0;
            file_.open(path, mode);
            if (file_) {
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

        explicit writer(const http::fields&, value_type& b);

        void init(boost::system::error_code& ec);

        boost::optional<std::pair<const_buffers_type, bool>> get(boost::system::error_code& ec);

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

        reader(const http::fields&, value_type& b);

        void init(boost::optional<std::uint64_t> const& content_length,
                  boost::system::error_code& ec);
        std::size_t put(const_buffers_type const& buffers, boost::system::error_code& ec);
        void finish(boost::system::error_code& ec);

    private:
        value_type& body_;
    };
};
} // namespace httplib::body