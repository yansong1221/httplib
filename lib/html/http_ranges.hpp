#pragma once
#include "httplib/config.hpp"
#include <cstdint>
#include <string_view>
#include <vector>

namespace httplib::html
{
    class http_ranges
    {
      public:
        using range_type = std::pair<int64_t, int64_t>;
        using ranges_type = std::vector<range_type>;

      public:
        static constexpr std::size_t max_ranges_default = 100;

      public:
        std::size_t size() const;
        bool empty() const;

        range_type const& front() const;
        range_type const& at(std::size_t index) const;

        void add(range_type const& range);

        ranges_type const& ranges() const;
        bool parse(std::string_view range_str, size_t file_size);

        std::size_t max_ranges() const;
        void set_max_ranges(std::size_t n);

      private:
        ranges_type ranges_;
        std::size_t max_ranges_ = max_ranges_default;
    };
} // namespace httplib::html