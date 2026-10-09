#include "state_store.hpp"
#include <boost/algorithm/string/trim.hpp>
#include <fstream>

namespace httplib::client
{
    using boost::algorithm::trim;

    state_store::state_store(fs::path save_path)
        : save_path_(std::move(save_path))
    {
    }

    fs::path
    state_store::path_for(fs::path const& save_path)
    {
        return fs::path(save_path.string() + ".dlstate");
    }

    void
    state_store::save(std::string const& url,
                      std::uint64_t content_length,
                      std::vector<std::uint64_t> const& seg_downloaded) const
    {
        std::ofstream f(path_for(save_path_), std::ios::trunc);
        if (!f.is_open())
        {
            return;
        }
        f << "url=" << url << '\n';
        f << "content_length=" << content_length << '\n';
        f << "segments=" << seg_downloaded.size() << '\n';
        for (std::size_t i = 0; i < seg_downloaded.size(); ++i)
        {
            f << "seg" << i << "_downloaded=" << seg_downloaded[i] << '\n';
        }
    }

    download_state
    state_store::load() const
    {
        download_state st;
        std::ifstream f(path_for(save_path_));
        if (!f.is_open())
        {
            return st;
        }
        std::string line;
        while (std::getline(f, line))
        {
            trim(line);
            if (line.empty())
            {
                continue;
            }
            auto eq = line.find('=');
            if (eq == std::string::npos)
            {
                continue;
            }
            auto key = line.substr(0, eq);
            auto val = line.substr(eq + 1);
            trim(key);
            trim(val);
            if (key == "url")
            {
                st.url = val;
            }
            else if (key == "content_length")
            {
                try
                {
                    st.content_length = std::stoull(val);
                }
                catch (...)
                {
                }
            }
            else if (key == "segments")
            {
                try
                {
                    st.segments = std::stoi(val);
                }
                catch (...)
                {
                }
            }
            else if (key.starts_with("seg") && key.ends_with("_downloaded"))
            {
                // Key shape: "seg<index>_downloaded". Keep per-segment progress
                // indexed so a gap in the file is never mistaken for progress.
                constexpr std::string_view suffix = "_downloaded";
                auto idx_str = key.substr(3, key.size() - 3 - suffix.size());
                try
                {
                    int idx = std::stoi(idx_str);
                    auto v = std::stoull(val);
                    if (idx >= 0)
                    {
                        auto uidx = static_cast<std::size_t>(idx);
                        if (uidx >= st.seg_downloaded.size())
                        {
                            st.seg_downloaded.resize(uidx + 1, 0);
                        }
                        st.seg_downloaded[uidx] = v;
                    }
                }
                catch (...)
                {
                }
            }
        }
        return st;
    }

    void
    state_store::clear() const
    {
        std::error_code ec;
        fs::remove(path_for(save_path_), ec);
    }

} // namespace httplib::client
