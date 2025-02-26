#include "httplib/body/any_body.hpp"

#include "httplib/body/compressor.hpp"

namespace httplib::body
{

void any_body::writer::init(boost::system::error_code& ec)
{
    proxy_->init(ec);
    if (!ec) return;
    compressor_ = compressor::create(compressor::mode::encode, content_encoding_);
}

boost::optional<std::pair<any_body::writer::const_buffers_type, bool>> any_body::writer::get(
    boost::system::error_code& ec)
{
    if (compressor_)
    {
        compressor_->consume();
        bool more = false;
        while (compressor_->buffer().size() == 0)
        {
            auto result = proxy_->get(ec);
            if (!result)
            {
                break;
            }
            more = result->second;
            compressor_->write(result->first, result->second);
        }

        auto decompressor = compressor::create(compressor::mode::decode, "gzip");
        decompressor->write(compressor_->buffer(), false);

        return {{compressor_->buffer(), more}};
    }

    return proxy_->get(ec);
}

any_body::writer::~writer() { proxy_.reset(); }

void any_body::reader::init(boost::optional<std::uint64_t> const& content_length, boost::system::error_code& ec)
{
    return proxy_->init(content_length, ec);
}
std::size_t any_body::reader::put(const_buffers_type const& buffers, boost::system::error_code& ec)
{
    return proxy_->put(buffers, ec);
}
void any_body::reader::finish(boost::system::error_code& ec) { return proxy_->finish(ec); }

any_body::reader::~reader() { }

} // namespace httplib::body
