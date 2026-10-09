#pragma once
#include "httplib/config.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>

namespace httplib::client
{

    /// Serializes a downloader's payload writes onto a single strand and owns
    /// the one output file of a run. The file is opened once via `open()`;
    /// `write` never reopens it. Every method hops onto `strand_` before doing
    /// the (synchronous) file work, and does not `co_await` inside the file
    /// body, so the operation runs atomically with respect to other strand
    /// handlers.
    class disk_writer
    {
      public:
        explicit disk_writer(net::any_io_executor ex);

        /// Opens the output file. When `truncate` is true the file is
        /// (re)created. `initial_offset` positions the cursor used by
        /// `write()`.
        net::awaitable<boost::system::error_code>
        open(fs::path const& path, bool truncate, std::uint64_t initial_offset);

        /// Sequential write at the current cursor.
        net::awaitable<boost::system::error_code> write(std::span<char const> data);

        net::awaitable<boost::system::error_code> close();

      private:
        net::strand<net::any_io_executor> strand_;
        std::ofstream out_;
    };

} // namespace httplib::client
