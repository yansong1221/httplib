#pragma once
#include "httplib/client/cache.hpp"
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
    /// the one output file of a run. Data is written to a sibling
    /// `<save_path>.part` file; `commit()` publishes it as the final
    /// `save_path`, while `close()` keeps the `.part` behind so a later run can
    /// resume. The file is opened once via `open()`; `write` never reopens it.
    /// Every method hops onto `strand_` before doing the (synchronous) file
    /// work, and does not `co_await` inside the file body, so the operation runs
    /// atomically with respect to other strand handlers.
    class disk_writer
    {
      public:
        explicit disk_writer(net::any_io_executor ex);

        /// Size in bytes of the in-progress `<save_path>.part` file, or 0 when
        /// there is none. Lets a caller pick the resume offset before `open()`.
        static std::uint64_t partial_size(fs::path const& save_path);

        /// Deletes the in-progress `<save_path>.part` file, if any.
        static void discard(fs::path const& save_path);

        /// Copies the cached `src` entry's body to `save_path` through the part
        /// file, so the destination only appears once it is fully written.
        /// Fails cleanly when the body has become unreadable.
        static net::awaitable<boost::system::error_code> copy_atomic(cache::entry const& src,
                                                                     fs::path const& save_path);

        /// Opens `<path>.part` for writing. When `truncate` is true the part file
        /// is (re)created; otherwise it is kept and the cursor starts at
        /// `initial_offset`.
        net::awaitable<boost::system::error_code> open(fs::path const& path,
                                                       bool truncate,
                                                       std::uint64_t initial_offset);

        /// Sequential write at the current cursor.
        net::awaitable<boost::system::error_code> write(std::span<char const> data);

        /// Closes the file, leaving the `.part` behind for a later resume.
        net::awaitable<boost::system::error_code> close();

        /// Closes the file and renames `<path>.part` to the final `path`,
        /// overwriting any existing destination.
        net::awaitable<boost::system::error_code> commit();

      private:
        net::strand<net::any_io_executor> strand_;
        std::ofstream out_;
        fs::path path_;
    };

} // namespace httplib::client
