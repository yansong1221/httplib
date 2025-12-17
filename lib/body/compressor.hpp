#pragma once
#include "httplib/config.hpp"
#include <boost/asio/buffer.hpp>
#include <functional>
#include <unordered_map>

namespace httplib::body {
class compressor
{
public:
    using ptr = std::unique_ptr<compressor, std::function<void(compressor*)>>;

    enum class mode
    {
        encode,
        decode,
    };
    virtual ~compressor()     = default;
    virtual void init(mode m) = 0;

    virtual net::const_buffer buffer() const                              = 0;
    virtual void write(const net::const_buffer& buffer, bool more = true) = 0;
    virtual void finish()                                                 = 0;
    virtual void consume_all()                                            = 0;
    virtual void consume(std::size_t bytes)                               = 0;
};

class compressor_factory
{
public:
    using create_function = std::function<compressor::ptr()>;

    const std::vector<std::string>& supported_encoding() const;

    compressor::ptr create(const std::string& encoding);

    bool is_supported_encoding(std::string_view encoding) const;

public:
    static compressor_factory& instance();

private:
    compressor_factory();
    void register_compressor(const std::string& encoding, create_function&& func);
    std::unordered_map<std::string, create_function> creators_;
};
} // namespace httplib::body