#include "disk_writer.h"
#include <boost/asio/dispatch.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/errc.hpp>
#include <utility>

namespace httplib::client
{

    disk_writer::disk_writer(net::any_io_executor ex)
        : strand_(net::make_strand(std::move(ex)))
    {
    }

    net::awaitable<boost::system::error_code>
    disk_writer::open(fs::path const& path, bool truncate, std::uint64_t initial_offset)
    {
        co_await net::dispatch(strand_, net::use_awaitable);

        if (out_.is_open())
        {
            out_.close();
        }

        auto mode = std::ios::out | std::ios::binary;
        if (truncate)
        {
            mode |= std::ios::trunc;
        }
        out_.open(path, mode);
        if (!out_.is_open())
        {
            co_return boost::system::errc::make_error_code(boost::system::errc::permission_denied);
        }

        // Position the sequential cursor (at EOF when resuming into an existing
        // file).
        if (initial_offset > 0)
        {
            out_.seekp(static_cast<std::streamoff>(initial_offset));
            if (!out_)
            {
                out_.close();
                co_return boost::system::errc::make_error_code(boost::system::errc::io_error);
            }
        }

        co_return boost::system::error_code {};
    }

    net::awaitable<boost::system::error_code>
    disk_writer::write(std::span<char const> data)
    {
        co_await net::dispatch(strand_, net::use_awaitable);

        if (!out_.is_open())
        {
            co_return boost::system::errc::make_error_code(boost::system::errc::bad_file_descriptor);
        }
        out_.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out_)
        {
            co_return boost::system::errc::make_error_code(boost::system::errc::no_space_on_device);
        }
        co_return boost::system::error_code {};
    }

    net::awaitable<boost::system::error_code>
    disk_writer::close()
    {
        co_await net::dispatch(strand_, net::use_awaitable);

        if (out_.is_open())
        {
            out_.close();
        }
        co_return boost::system::error_code {};
    }

} // namespace httplib::client
