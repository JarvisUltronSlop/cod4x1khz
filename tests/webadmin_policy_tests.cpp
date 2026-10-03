#include <webadmin_policy.hpp>
#include <cstdio>
#include <cstdlib>
#include <limits>
#define CHECK(expression) do { if (!(expression)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); std::exit(1); } } while (0)
namespace web = khz::web;
static std::string post(std::string body, std::string extra = {}) {
    return "POST /webadmin?action=sendcmd HTTP/1.1\r\nHost: 127.0.0.1:28980\r\nOrigin: http://127.0.0.1:28980\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra + "\r\n" + body;
}
int main() {
    web::Parsed request;
    CHECK(web::Parse("GET /status HTTP/1.0\r\n\r\n", &request) == web::ParseResult::Valid);
    const std::string get = "GET /status HTTP/1.1\r\nHost: 127.0.0.1:28980\r\nX-Test_2: valid\r\n\r\n";
    for (unsigned i = 0; i < get.size(); ++i) CHECK(web::Parse(std::string_view(get).substr(0, i), &request) == web::ParseResult::Incomplete);
    CHECK(web::Parse(get, &request) == web::ParseResult::Valid); CHECK(!request.post && request.target == "/status");
    CHECK(web::SameOrigin(request, "127.0.0.1", 28980)); CHECK(!web::SameOrigin(request, "192.168.1.5", 28980));
    const auto write = post("consolecommand=map+mp_crash&csrf=" + std::string(64, 'a'));
    CHECK(web::Parse(write.substr(0, write.size() - 1), &request) == web::ParseResult::Incomplete);
    CHECK(web::Parse(write, &request) == web::ParseResult::Valid);
    CHECK(request.post && web::Value(request.form, "consolecommand") == "map mp_crash");
    CHECK(web::Value(request.query, "action") == "sendcmd"); CHECK(web::SameOrigin(request, "127.0.0.1", 28980));
    request.origin = "http://attacker.test"; CHECK(!web::SameOrigin(request, "127.0.0.1", 28980));
    request.origin = "http://127.0.0.1:28980/"; CHECK(!web::SameOrigin(request, "127.0.0.1", 28980));
    for (const char* invalid : {
        "GET /status HTTP/1.1\r\n\r\n", "GET /status HTTP/1.1\r\nHost: x\r\nHost: x\r\n\r\n",
        "GET /status HTTP/1.1\r\nHost: x\r\n folded: no\r\n\r\n", "GET /status HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: identity\r\n\r\n",
        "GET /status HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\nx", "GET /status HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\n\r\n",
        "POST /webadmin HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n", "GET http://x/status HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET /status#x HTTP/1.1\r\nHost: x\r\n\r\n", "GET /status extra HTTP/1.1\r\nHost: x\r\n\r\n"})
        CHECK(web::Parse(invalid, &request) == web::ParseResult::Invalid);
    CHECK(web::Parse(post("x=y", "Content-Length: 3\r\n"), &request) == web::ParseResult::Invalid);
    CHECK(web::Parse(post("x=y", "Transfer-Encoding: chunked\r\n"), &request) == web::ParseResult::Invalid);
    CHECK(web::Parse(std::string(web::MaxHeader + 1, 'x'), &request) == web::ParseResult::TooLarge);
    CHECK(web::Parse(post(std::string(web::MaxBody + 1, 'x')), &request) == web::ParseResult::Invalid);
    CHECK(web::Parse(get + std::string(web::MaxHeader + web::MaxBody, 'x'), &request) == web::ParseResult::TooLarge);
    web::Form form;
    CHECK(web::ParseForm("reason=a%26b%3Dc&name=%3Cscript%3E", &form)); CHECK(web::Value(form, "reason") == "a&b=c");
    for (const char* invalid : {"x=y&x=z", "=y", "x", "x=%", "x=%00", "x=%0D%0A", "x=%7f", "x=y&", "x=abc\n"}) CHECK(!web::ParseForm(invalid, &form));
    CHECK(!web::ParseForm("x=" + std::string(1024, 'a'), &form));
    std::string many; for (unsigned i = 0; i != 32; ++i) many += (i ? "&" : "") + std::string("x") + std::to_string(i) + "=a";
    CHECK(web::ParseForm(many, &form)); CHECK(!web::ParseForm(many + "&extra=a", &form));
    std::uint64_t number = 7;
    CHECK(web::Decimal("18446744073709551615", UINT64_MAX, &number) && number == UINT64_MAX);
    for (const char* invalid : {"18446744073709551616", "184467440737095516159", "-1", "+1", "1x", "", " 1"}) {
        number = 7; CHECK(!web::Decimal(invalid, UINT64_MAX, &number)); CHECK(number == 7);
    }
    CHECK(web::Decimal("15", 15, &number) && number == 15); CHECK(!web::Decimal("16", 15, &number));
    const std::string token(64, 'a'); std::string cookie;
    CHECK(web::Cookie("other=x; SessionId=" + token, &cookie) && cookie == token);
    CHECK(web::Cookie("other=x", &cookie) && cookie.empty());
    CHECK(!web::Cookie("SessionId=" + token + "; SessionId=" + token, &cookie));
    CHECK(!web::Cookie("SessionId=" + std::string(64, 'A'), &cookie)); CHECK(!web::Cookie("SessionId=short", &cookie));
    CHECK(web::Escape("<script a=\"x\">&'b'") == "&lt;script a=&quot;x&quot;&gt;&amp;&#39;b&#39;");
    for (const char* valid : {"127.0.0.1", "127.2.3.4", "10.0.0.1", "192.168.1.2", "172.16.0.1", "172.31.255.255"}) CHECK(web::PrivateAddress(valid));
    for (const char* invalid : {"0.0.0.0", "8.8.8.8", "172.15.0.1", "172.32.0.1", "192.169.1.2", "127.00.0.1", "127.0.0", "127.0.0.1.2", "127.0.0.256", "localhost", "::1"}) CHECK(!web::PrivateAddress(invalid));
    for (const char* valid : {"style.css", "nested/logo.svg", "a_b-2.js"}) CHECK(web::StaticPath(valid));
    for (const char* invalid : {"", "../secret", "/style.css", "a//b", "a/", "a\\b", "a:b", "a.", "CON.css", "nul", "com1.js", "lpt9.svg", "%2e%2e/secret", "a/b/c/d/e/f/g/h/i"}) CHECK(!web::StaticPath(invalid));
    std::puts("webadmin HTTP policy tests passed");
}
