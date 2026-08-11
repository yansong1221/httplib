#include "httplib/html/form_data.hpp"
#include "httplib/html/http_ranges.hpp"
#include "httplib/html/query_params.hpp"
#include "httplib/util/misc.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using namespace std::string_view_literals;

TEST_CASE("query_params: decode URL-encoded string", "[body-utils]")
{
    httplib::html::query_params qp;
    REQUIRE(qp.decode("name=John+Doe&age=30&active=true"));

    REQUIRE(qp.at("name") == "John+Doe");
    REQUIRE(qp.at("age") == "30");
    REQUIRE(qp.at("active") == "true");
}

TEST_CASE("query_params: at_number returns integer", "[body-utils]")
{
    httplib::html::query_params qp;
    REQUIRE(qp.decode("count=42&neg=-17"));

    REQUIRE(qp.at<int64_t>("count") == 42);
    REQUIRE(qp.at<int64_t>("neg") == -17);
}

TEST_CASE("query_params: at_bool returns boolean", "[body-utils]")
{
    httplib::html::query_params qp;
    REQUIRE(qp.decode("flag=true&off=false&yes=1&no=0"));

    REQUIRE(qp.at<bool>("flag"));
    REQUIRE_FALSE(qp.at<bool>("off"));
    REQUIRE(qp.at<bool>("yes"));
    REQUIRE_FALSE(qp.at<bool>("no"));
}

TEST_CASE("query_params: all returns multiple values", "[body-utils]")
{
    httplib::html::query_params qp;
    REQUIRE(qp.decode("tag=cpp&tag=rust&tag=python"));

    auto tags = qp.all("tag");
    REQUIRE(tags.size() == 3);
    REQUIRE(tags[0] == "cpp");
    REQUIRE(tags[1] == "rust");
    REQUIRE(tags[2] == "python");
}

TEST_CASE("query_params: exists check", "[body-utils]")
{
    httplib::html::query_params qp;
    REQUIRE(qp.decode("key=val"));

    REQUIRE(qp.has("key"));
    REQUIRE_FALSE(qp.has("missing"));
}

TEST_CASE("query_params: empty check", "[body-utils]")
{
    httplib::html::query_params qp;
    REQUIRE(qp.empty());

    qp.decode("a=1");
    REQUIRE_FALSE(qp.empty());
}

TEST_CASE("query_params: add and encode round-trip", "[body-utils]")
{
    httplib::html::query_params qp;
    qp.add("hello", "world");
    qp.add("foo", "bar");

    auto encoded = qp.encoded();
    REQUIRE(encoded.find("hello=world") != std::string::npos);
    REQUIRE(encoded.find("foo=bar") != std::string::npos);

    httplib::html::query_params qp2;
    REQUIRE(qp2.decode(encoded));
    REQUIRE(qp2.at("hello") == "world");
    REQUIRE(qp2.at("foo") == "bar");
}

TEST_CASE("query_params: at_number throws on invalid input", "[body-utils]")
{
    httplib::html::query_params qp;
    REQUIRE(qp.decode("val=not-a-number"));

    REQUIRE_THROWS(qp.at<int64_t>("val"));
}

TEST_CASE("form_data: field has_data and is_file", "[body-utils]")
{
    httplib::html::form_data::field f;
    f.name = "username";
    f.content = "john";
    f.filename = "";

    REQUIRE(f.has_data());
    REQUIRE_FALSE(f.is_file());

    httplib::html::form_data::field f2;
    f2.name = "avatar";
    f2.filename = "photo.png";
    f2.content = "binary-data";
    f2.content_type = "image/png";

    REQUIRE(f2.has_data());
    REQUIRE(f2.is_file());
}

TEST_CASE("form_data: field_by_name lookup", "[body-utils]")
{
    httplib::html::form_data fd;
    fd.fields.push_back({ "name", "", "text/plain", "Alice" });
    fd.fields.push_back({ "email", "", "text/plain", "alice@example.com" });

    auto name_field = fd.field_by_name("name");
    REQUIRE(name_field.has_value());
    REQUIRE(name_field->content == "Alice");

    auto email_field = fd.field_by_name("email");
    REQUIRE(email_field.has_value());
    REQUIRE(email_field->content == "alice@example.com");

    auto missing = fd.field_by_name("missing");
    REQUIRE_FALSE(missing.has_value());
}

TEST_CASE("form_data: has_data and has_content", "[body-utils]")
{
    httplib::html::form_data fd;
    fd.fields.push_back({ "key1", "", "", "value1" });
    fd.fields.push_back({ "key2", "f.txt", "", "file content" });

    REQUIRE(fd.has_data("key1"));
    REQUIRE(fd.has_content("key1"));
    REQUIRE(fd.has_data("key2"));
    REQUIRE(fd.has_content("key2"));
    REQUIRE_FALSE(fd.has_data("missing"));
    REQUIRE_FALSE(fd.has_content("missing"));
}

TEST_CASE("form_data: content retrieval", "[body-utils]")
{
    httplib::html::form_data fd;
    fd.fields.push_back({ "greeting", "", "", "hello world" });

    auto c = fd.content("greeting");
    REQUIRE(c.has_value());
    REQUIRE(*c == "hello world");

    auto c2 = fd.content("missing");
    REQUIRE_FALSE(c2.has_value());
}

TEST_CASE("form_data: dump", "[body-utils]")
{
    httplib::html::form_data fd;
    fd.fields.push_back({ "key1", "", "", "val1" });
    fd.fields.push_back({ "key2", "file.txt", "text/plain", "content" });

    auto d = fd.dump();
    REQUIRE(d.find("key1") != std::string::npos);
    REQUIRE(d.find("val1") != std::string::npos);
    REQUIRE(d.find("file.txt") != std::string::npos);
}

TEST_CASE("util::url_decode", "[body-utils]")
{
    REQUIRE(httplib::util::url_decode("hello%20world") == "hello world");
    REQUIRE(httplib::util::url_decode("%2Fpath%2Fto%2Ffile") == "/path/to/file");
    REQUIRE(httplib::util::url_decode("noencoding") == "noencoding");
    REQUIRE(httplib::util::url_decode("%41%42%43") == "ABC");
}

TEST_CASE("util::url_decode rejects malformed percent encoding", "[body-utils]")
{
    REQUIRE(httplib::util::url_decode("trailing%") == "trailing%");
    REQUIRE(httplib::util::url_decode("incomplete%2") == "incomplete%2");
    REQUIRE(httplib::util::url_decode("%ZZ") == "%ZZ");
    REQUIRE(httplib::util::url_decode("%%") == "%%");
    REQUIRE(httplib::util::url_decode("%GG") == "%GG");
    REQUIRE(httplib::util::url_decode("a%20b%c") == "a b%c");
    REQUIRE(httplib::util::url_decode("%") == "%");
    REQUIRE(httplib::util::url_decode("") == "");
}

TEST_CASE("util::url_decode does not read out of bounds", "[body-utils]")
{
    std::string s("%");
    httplib::util::url_decode(s);
    REQUIRE(s == "%");

    std::string s2("%A");
    httplib::util::url_decode(s2);
    REQUIRE(s2 == "%A");

    std::string s3("%20%");
    httplib::util::url_decode(s3);
    REQUIRE(s3 == " %");

    std::string s4("%20%A");
    httplib::util::url_decode(s4);
    REQUIRE(s4 == " %A");
}

TEST_CASE("util::url_encode", "[body-utils]")
{
    REQUIRE(httplib::util::url_encode("hello world") == "hello+world");
    REQUIRE(httplib::util::url_encode("/path/to/file") == "%2fpath%2fto%2ffile");
    REQUIRE(httplib::util::url_encode("alpha123-_.~") == "alpha123-_.~");
}

TEST_CASE("util::url_encode round-trip", "[body-utils]")
{
    std::string original = "hello%20world%21";
    auto decoded = httplib::util::url_decode(std::string_view(original));
    REQUIRE(decoded == "hello world!");
}

TEST_CASE("util::split basic", "[body-utils]")
{
    auto parts = httplib::util::split("a,b,c", ",");
    REQUIRE(parts.size() == 3);
    REQUIRE(parts[0] == "a");
    REQUIRE(parts[1] == "b");
    REQUIRE(parts[2] == "c");
}

TEST_CASE("util::split with empty string", "[body-utils]")
{
    auto parts = httplib::util::split("", ",");
    REQUIRE(parts.empty());
}

TEST_CASE("http_ranges: parse single range", "[body-utils]")
{
    httplib::html::http_ranges ranges;
    REQUIRE(ranges.parse("bytes=0-99", 1000));
    REQUIRE(ranges.size() == 1);
    auto r = ranges.front();
    REQUIRE(r.first == 0);
    REQUIRE(r.second == 99);
}

TEST_CASE("http_ranges: parse multiple ranges", "[body-utils]")
{
    httplib::html::http_ranges ranges;
    REQUIRE(ranges.parse("bytes=0-49,100-149", 1000));
    REQUIRE(ranges.size() == 2);

    auto r0 = ranges.at(0);
    REQUIRE(r0.first == 0);
    REQUIRE(r0.second == 49);

    auto r1 = ranges.at(1);
    REQUIRE(r1.first == 100);
    REQUIRE(r1.second == 149);
}

TEST_CASE("http_ranges: open-ended range", "[body-utils]")
{
    httplib::html::http_ranges ranges;
    REQUIRE(ranges.parse("bytes=950-", 1000));
    REQUIRE(ranges.size() == 1);
    auto r = ranges.front();
    REQUIRE(r.first == 950);
    REQUIRE(r.second == 999);
}

TEST_CASE("http_ranges: suffix range", "[body-utils]")
{
    httplib::html::http_ranges ranges;
    REQUIRE(ranges.parse("bytes=-100", 1000));
    REQUIRE(ranges.size() == 1);
    auto r = ranges.front();
    REQUIRE(r.first == 900);
    REQUIRE(r.second == 999);
}

TEST_CASE("http_ranges: suffix range small", "[body-utils]")
{
    httplib::html::http_ranges ranges;
    REQUIRE(ranges.parse("bytes=-4", 10));
    REQUIRE(ranges.size() == 1);
    auto r = ranges.front();
    REQUIRE(r.first == 6);
    REQUIRE(r.second == 9);
}

TEST_CASE("http_ranges: empty check", "[body-utils]")
{
    httplib::html::http_ranges ranges;
    REQUIRE(ranges.empty());
    REQUIRE(ranges.parse("bytes=0-99", 1000));
    REQUIRE_FALSE(ranges.empty());
}

TEST_CASE("http_ranges: append", "[body-utils]")
{
    httplib::html::http_ranges ranges;
    ranges.add({ 0, 99 });
    ranges.add({ 200, 299 });
    REQUIRE(ranges.size() == 2);
}
