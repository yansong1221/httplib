#pragma once
#include "httplib/client/client_fwd.hpp"
#include <boost/beast/http/field.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace httplib::client
{

    class HTTPLIB_API cache
    {
      public:
        struct entry
        {
            fs::path body_path;
            http::fields headers;
            std::uint64_t body_size = 0;
        };

        virtual ~cache() = default;

        cache(cache const&) = delete;
        cache& operator=(cache const&) = delete;
        cache(cache&&) = default;
        cache& operator=(cache&&) = default;

        virtual std::optional<entry> get(std::string_view url) = 0;
        virtual void put(std::string_view url, http::fields const& headers, fs::path const& src_body) = 0;
        virtual void remove(std::string_view url) = 0;

      protected:
        cache() = default;
    };

} // namespace httplib::client
