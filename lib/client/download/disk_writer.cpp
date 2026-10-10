#include "disk_writer.h"
#include <boost/asio/dispatch.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/errc.hpp>
#include <utility>

namespace httplib::client
{
    namespace
    {
        // Everything is written to a sibling part file and renamed in place on
        // success, so a failed run never leaves a complete-looking `save_path`.
        fs::path
        part_path_of(fs::path const& save_path)
        {
            fs::path p = save_path;
            p += ".part";
            return p;
        }

        // rename() does not overwrite on Windows, so drop the destination first.
        boost::system::error_code
        publish(fs::path const& part_path, fs::path const& save_path)
        {
            std::error_code ec;
            fs::remove(save_path, ec);
            ec.clear();
            fs::rename(part_path, save_path, ec);
            if (ec)
            {
                return boost::system::errc::make_error_code(boost::system::errc::io_error);
            }
            return {};
        }
    } // namespace

    disk_writer::disk_writer(net::any_io_executor ex) : strand_(net::make_strand(std::move(ex))) {}

    std::uint64_t
    disk_writer::partial_size(fs::path const& save_path)
    {
        std::error_code ec;
        auto const sz = fs::file_size(part_path_of(save_path), ec);
        return ec ? 0 : static_cast<std::uint64_t>(sz);
    }

    void
    disk_writer::discard(fs::path const& save_path)
    {
        std::error_code ec;
        fs::remove(part_path_of(save_path), ec);
    }

    net::awaitable<boost::system::error_code>
    disk_writer::copy_atomic(cache::entry const& src, fs::path const& save_path)
    {
        fs::path const part_path = part_path_of(save_path);
        if (auto ec = src.copy_to_file(part_path); ec)
        {
            std::error_code rm_ec;
            fs::remove(part_path, rm_ec);
            co_return boost::system::errc::make_error_code(boost::system::errc::io_error);
        }
        co_return publish(part_path, save_path);
    }

    net::awaitable<boost::system::error_code>
    disk_writer::open(fs::path const& path, bool truncate, std::uint64_t initial_offset)
    {
        co_await net::dispatch(strand_, net::use_awaitable);

        if (out_.is_open())
        {
            out_.close();
        }
        path_ = path;

        auto mode = std::ios::out | std::ios::binary;
        if (truncate)
        {
            mode |= std::ios::trunc;
        }
        out_.open(part_path_of(path), mode);
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

    net::awaitable<boost::system::error_code>
    disk_writer::commit()
    {
        co_await net::dispatch(strand_, net::use_awaitable);

        if (out_.is_open())
        {
            out_.close();
        }
        if (path_.empty())
        {
            co_return boost::system::error_code {};
        }
        co_return publish(part_path_of(path_), path_);
    }

} // namespace httplib::client
