#pragma once
#include "httplib/config.hpp"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace httplib::client
{

    /// Persisted resumable-download state: the canonical URL, total size, segment
    /// count, and bytes already written per segment. A stale or inconsistent
    /// file from another download is rejected by comparing all three of these.
    struct download_state
    {
        std::string url;
        std::uint64_t content_length = 0;
        int segments = 0;
        /// Bytes persisted per segment (indexed by segment). Empty or short
        /// vectors are treated as "no progress".
        std::vector<std::uint64_t> seg_downloaded;
    };

    /// Reads/writes the `.dlstate` sidecar file next to the download target.
    /// Owns only the path; every operation is synchronous and best-effort
    /// (missing or malformed files load as empty state).
    class state_store
    {
      public:
        explicit state_store(fs::path save_path);

        void save(std::string const& url,
                  std::uint64_t content_length,
                  std::vector<std::uint64_t> const& seg_downloaded) const;
        download_state load() const;
        void clear() const;

        static fs::path path_for(fs::path const& save_path);

      private:
        fs::path save_path_;
    };

} // namespace httplib::client
