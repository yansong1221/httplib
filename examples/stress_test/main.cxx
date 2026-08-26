#include "httplib/body/string_body.hpp"
#include "httplib/client/client.hpp"
#include <algorithm>
#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/program_options.hpp>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <iostream>
#include <map>
#include <numeric>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace std::string_view_literals;
namespace http = httplib::http;
namespace net = httplib::net;
namespace po = boost::program_options;

struct stress_config
{
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    std::string path = "/";
    std::string body;
    std::string method = "GET";
    std::string url;
    http::verb verb = http::verb::get;
    int threads = 2;
    int connections = 10;
    int duration_sec = 10;
    int request_count = 0;
    int timeout_sec = 10;
    std::vector<std::string> headers;
};

struct conn_stats
{
    int64_t requests = 0;
    int64_t bytes = 0;
    int64_t err_connect = 0;
    int64_t err_timeout = 0;
    int64_t err_other = 0;
    std::map<int, int64_t> status_codes;
    std::vector<double> latencies;
};

static std::string
format_size(int64_t bytes)
{
    if (bytes < 1024)
    {
        return std::format("{}B", bytes);
    }
    if (bytes < 1024 * 1024)
    {
        return std::format("{:.2f}KB", bytes / 1024.0);
    }
    return std::format("{:.2f}MB", bytes / (1024.0 * 1024.0));
}

static std::string
format_req_sec(double rps)
{
    if (rps >= 1000)
    {
        return std::format("{:.2f}k", rps / 1000.0);
    }
    return std::format("{:.2f}", rps);
}

static void
parse_url(std::string const& url, std::string& host, uint16_t& port, std::string& path)
{
    std::string_view sv = url;
    if (sv.starts_with("https://"))
    {
        sv.remove_prefix(8);
        port = 443;
    }
    else if (sv.starts_with("http://"))
    {
        sv.remove_prefix(7);
        port = 80;
    }

    auto slash = sv.find('/');
    std::string_view host_port = (slash == std::string_view::npos) ? sv : sv.substr(0, slash);
    path = (slash == std::string_view::npos) ? "/" : std::string(sv.substr(slash));

    auto colon = host_port.rfind(':');
    if (colon != std::string_view::npos && colon > 0)
    {
        host = std::string(host_port.substr(0, colon));
        auto p = host_port.substr(colon + 1);
        auto val = 0;
        auto [_, ec] = std::from_chars(p.data(), p.data() + p.size(), val);
        if (ec == std::errc {})
        {
            port = static_cast<uint16_t>(val);
        }
    }
    else
    {
        host = std::string(host_port);
    }
}

static double
percentile(std::vector<double> const& sorted, double p)
{
    if (sorted.empty())
    {
        return 0;
    }
    auto idx = static_cast<size_t>(p / 100.0 * (sorted.size() - 1));
    return sorted[std::min(idx, sorted.size() - 1)];
}

static void
print_report(stress_config const& cfg, std::vector<conn_stats> const& conns, std::chrono::nanoseconds total_elapsed)
{
    int64_t total_req = 0;
    int64_t total_bytes = 0;
    int64_t total_err = 0;
    std::vector<double> all_latencies;
    std::map<int, int64_t> status_map;

    for (auto& cs : conns)
    {
        total_req += cs.requests;
        total_bytes += cs.bytes;
        total_err += cs.err_connect + cs.err_timeout + cs.err_other;
        all_latencies.insert(all_latencies.end(), cs.latencies.begin(), cs.latencies.end());
        for (auto& [code, cnt] : cs.status_codes)
        {
            status_map[code] += cnt;
        }
    }

    double elapsed_s = std::chrono::duration<double>(total_elapsed).count();
    double rps = elapsed_s > 0 ? total_req / elapsed_s : 0;
    double tps = elapsed_s > 0 ? total_bytes / elapsed_s : 0;

    std::sort(all_latencies.begin(), all_latencies.end());

    // Group connections into thread buckets for Thread Stats
    int conns_per = std::max(1, cfg.connections / cfg.threads);
    std::vector<double> group_avgs, group_rps;
    for (int t = 0; t < cfg.threads; ++t)
    {
        int64_t g_req = 0;
        double g_sum = 0;
        int g_count = 0;
        for (int c = t * conns_per; c < std::min((t + 1) * conns_per, cfg.connections); ++c)
        {
            auto& cs = conns[c];
            g_req += cs.requests;
            g_count += static_cast<int>(cs.latencies.size());
            for (auto v : cs.latencies)
            {
                g_sum += v;
            }
        }
        group_avgs.push_back(g_count > 0 ? g_sum / g_count : 0);
        group_rps.push_back(elapsed_s > 0 ? g_req / elapsed_s : 0);
    }

    auto mean_of = [](std::vector<double> const& v)
    { return v.empty() ? 0.0 : std::accumulate(v.begin(), v.end(), 0.0) / v.size(); };
    auto stdev_of = [](std::vector<double> const& v, double m)
    {
        if (v.size() < 2)
        {
            return 0.0;
        }
        double sq = 0;
        for (auto x : v)
        {
            sq += (x - m) * (x - m);
        }
        return std::sqrt(sq / (v.size() - 1));
    };
    auto pct_near = [](std::vector<double> const& v, double m, double s)
    {
        if (v.size() < 2 || s <= 0)
        {
            return 100.0;
        }
        double lo = m - s, hi = m + s;
        int n = 0;
        for (auto x : v)
        {
            if (x >= lo && x <= hi)
            {
                ++n;
            }
        }
        return 100.0 * n / v.size();
    };

    double avg_lat = mean_of(group_avgs);
    double stdev_lat = stdev_of(group_avgs, avg_lat);
    double max_lat = all_latencies.empty() ? 0 : all_latencies.back();
    double avg_req = mean_of(group_rps);
    double stdev_req = stdev_of(group_rps, avg_req);
    double max_req = group_rps.empty() ? 0 : *std::max_element(group_rps.begin(), group_rps.end());

    std::cout << "  Thread Stats   Avg      Stdev     Max   +/- Stdev\n";
    std::cout << std::format("    Latency     {:>5}ms {:>5}ms {:>5}ms {:>5.0f}%\n",
                             std::format("{:.2f}", avg_lat),
                             std::format("{:.2f}", stdev_lat),
                             std::format("{:.2f}", max_lat),
                             pct_near(group_avgs, avg_lat, stdev_lat));
    std::cout << std::format("    Req/Sec     {:>5}  {:>5}  {:>5}  {:>5.0f}%\n",
                             format_req_sec(avg_req),
                             format_req_sec(stdev_req),
                             format_req_sec(max_req),
                             pct_near(group_rps, avg_req, stdev_req));

    std::cout << "  Latency Distribution\n";
    std::cout << std::format("     50% {:>7.2f}ms\n", percentile(all_latencies, 50));
    std::cout << std::format("     75% {:>7.2f}ms\n", percentile(all_latencies, 75));
    std::cout << std::format("     90% {:>7.2f}ms\n", percentile(all_latencies, 90));
    std::cout << std::format("     99% {:>7.2f}ms\n", percentile(all_latencies, 99));

    std::cout << "  Status Codes\n";
    for (auto& [code, count] : status_map)
    {
        std::cout << std::format("     {}: {}\n", code, count);
    }

    std::cout << std::format("  {} requests in {:.2f}s, {} read\n", total_req, elapsed_s, format_size(total_bytes));

    if (total_err > 0)
    {
        int64_t e_conn = 0, e_timeout = 0, e_other = 0;
        for (auto& cs : conns)
        {
            e_conn += cs.err_connect;
            e_timeout += cs.err_timeout;
            e_other += cs.err_other;
        }
        std::cout << std::format("  Socket errors: connect {}, read 0, write 0, timeout {}, other {}\n",
                                 e_conn,
                                 e_timeout,
                                 e_other);
    }

    std::cout << std::format("Requests/sec: {:>8.2f}\n", rps);
    std::cout << std::format("Transfer/sec: {:>8}\n", format_size(static_cast<int64_t>(tps)));
}

int
main(int argc, char** argv)
{
    stress_config cfg;

    po::options_description desc("httplib Stress Test Client\n\nOptions");
    desc.add_options()               //
        ("help,h", "Show this help") //
        ("url",
         po::value<std::string>(&cfg.url)->default_value("http://127.0.0.1:8080/"),
         "Target URL (e.g. http://host:port/path)")("body",
                                                    po::value<std::string>(&cfg.body)->default_value(""),
                                                    "Request body") //
        ("method,X",
         po::value<std::string>(&cfg.method)->default_value("GET"),
         "HTTP method")                                                                    //
        ("threads,t", po::value<int>(&cfg.threads)->default_value(2), "Number of threads") //
        ("connections,c",
         po::value<int>(&cfg.connections)->default_value(10),
         "Total TCP connections") //
        ("duration,d",
         po::value<int>(&cfg.duration_sec)->default_value(10),
         "Duration in seconds") //
        ("requests,n",
         po::value<int>(&cfg.request_count)->default_value(0),
         "Total requests (overrides --duration when > 0)") //
        ("timeout", po::value<int>(&cfg.timeout_sec)->default_value(10), "Request timeout in seconds")(
            "header,H",
            po::value<std::vector<std::string>>(&cfg.headers)->multitoken(),
            "Add header (e.g. -H \"Content-Type: application/json\")");

    po::variables_map vm;
    try
    {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    }
    catch (po::error const& e)
    {
        std::cerr << "Error: " << e.what() << "\n\n" << desc << "\n";
        return 1;
    }

    if (vm.count("help"))
    {
        std::cout << desc << "\n";
        return 0;
    }

    parse_url(cfg.url, cfg.host, cfg.port, cfg.path);

    auto upper = cfg.method;
    for (auto& c : upper)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    cfg.verb = http::string_to_verb(upper);
    if (cfg.threads <= 0)
    {
        cfg.threads = 1;
    }
    if (cfg.connections <= 0)
    {
        cfg.connections = cfg.threads;
    }

    http::fields req_headers;
    for (auto& h : cfg.headers)
    {
        auto pos = h.find(':');
        if (pos != std::string::npos)
        {
            auto name = h.substr(0, pos);
            auto value = h.substr(pos + 1);
            if (!value.empty() && value.front() == ' ')
            {
                value.erase(0, 1);
            }
            req_headers.set(name, value);
        }
    }
    if (cfg.connections < cfg.threads)
    {
        cfg.connections = cfg.threads;
    }

    bool count_mode = (cfg.request_count > 0);

    if (count_mode)
    {
        std::cout << std::format("Running {} requests test @ {}\n", cfg.request_count, cfg.url);
    }
    else
    {
        std::cout << std::format("Running {}s test @ {}\n", cfg.duration_sec, cfg.url);
    }
    std::cout << std::format("  {} threads and {} connections\n", cfg.threads, cfg.connections);
    std::cout << std::format("  {} {}\n", cfg.method, cfg.path);
    std::cout << std::format("  body: '{}'\n\n", cfg.body.empty() ? "<empty>"sv : cfg.body);

    net::thread_pool pool(cfg.threads);
    std::vector<conn_stats> conns(cfg.connections);
    std::atomic<bool> stop_flag { false };
    std::atomic<int64_t> remaining { cfg.request_count };

    auto overall_start = std::chrono::steady_clock::now();

    for (int i = 0; i < cfg.connections; ++i)
    {
        net::co_spawn(
            pool.get_executor(),
            [&, i]() -> net::awaitable<void>
            {
                auto& cs = conns[i];
                httplib::client::http_client client(pool.get_executor(), cfg.host, cfg.port);
                client.set_timeout(std::chrono::seconds(cfg.timeout_sec));

                while (true)
                {
                    if (count_mode)
                    {
                        if (remaining.fetch_sub(1, std::memory_order_relaxed) <= 0)
                        {
                            break;
                        }
                    }
                    else
                    {
                        if (stop_flag.load(std::memory_order_relaxed))
                        {
                            break;
                        }
                    }

                    auto t0 = std::chrono::steady_clock::now();

                    httplib::client::http_client::response_result result;
                    auto req = httplib::client::request(cfg.verb, cfg.path, req_headers);
                    if (!cfg.body.empty())
                    {
                        req.set_body(cfg.body);
                    }
                    result = co_await client.async_send_request(std::move(req));

                    auto t1 = std::chrono::steady_clock::now();
                    double lat_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

                    if (result.has_value())
                    {
                        cs.requests++;
                        cs.status_codes[result.value().result_int()]++;
                        try
                        {
                            cs.bytes += result.value().as_string().size();
                        }
                        catch (std::bad_variant_access const&)
                        {
                        }
                    }
                    else
                    {
                        auto ec = result.error();
                        if (ec == boost::system::errc::timed_out || ec == boost::system::errc::operation_would_block)
                        {
                            cs.err_timeout++;
                        }
                        else if (ec == boost::system::errc::connection_refused
                                 || ec == boost::system::errc::not_connected
                                 || ec == boost::system::errc::host_unreachable)
                        {
                            cs.err_connect++;
                        }
                        else
                        {
                            cs.err_other++;
                            if (cs.err_other == 1)
                            {
                                std::cerr << std::format("err: {} value={} msg={}\n",
                                                         ec.category().name(),
                                                         ec.value(),
                                                         ec.message());
                            }
                        }
                    }

                    cs.latencies.push_back(lat_ms);
                }
                co_return;
            },
            net::detached);
    }

    if (!count_mode)
    {
        std::this_thread::sleep_for(std::chrono::seconds(cfg.duration_sec));
        stop_flag.store(true, std::memory_order_relaxed);
    }

    pool.join();

    auto overall_elapsed = std::chrono::steady_clock::now() - overall_start;
    print_report(cfg, conns, overall_elapsed);

    return 0;
}
