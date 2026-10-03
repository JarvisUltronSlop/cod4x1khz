#include <plugin_io_policy.hpp>
#include <cstdio>
#include <cstdlib>
#include <memory>
#define CHECK(expression) do { if (!(expression)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression); std::exit(1); } } while (0)
namespace io = khz::plugins::io;
int main() {
    char body[1024]{}; CHECK(io::AppendForm(body, sizeof(body), "command", "HELO"));
    CHECK(io::AppendForm(body, sizeof(body), "name", "A+B &=%25"));
    CHECK(!std::strcmp(body, "command=HELO&name=A%2BB+%26%3D%2525"));
    auto fields = std::make_unique<io::Form>(); CHECK(io::ParseForm(body, fields.get()));
    CHECK(!std::strcmp(io::Value(*fields, "COMMAND"), "HELO")); CHECK(!std::strcmp(io::Value(*fields, "name"), "A+B &=%25"));
    CHECK(!std::strcmp(io::Value(*fields, "name"), "A+B &=%25")); // repeated access never double decodes
    CHECK(!*io::Value(*fields, "missing"));
    CHECK(io::ParseForm("reason=hello%0Aworld&duplicate=first&duplicate=second\r\nignored=1", fields.get()));
    CHECK(!std::strcmp(io::Value(*fields, "reason"), "hello\nworld")); CHECK(!std::strcmp(io::Value(*fields, "duplicate"), "first"));
    for (const char* malformed : {"bad", "=empty", "a=%00", "a=%", "a=%zz"}) { CHECK(!io::ParseForm(malformed, fields.get())); CHECK(!*io::Value(*fields, "a")); }
    std::string many; for (unsigned i = 0; i != 32; ++i) many += (i ? "&" : "") + std::string("k") + std::to_string(i) + "=value";
    CHECK(io::ParseForm(many, fields.get())); CHECK(!io::ParseForm(many + "&extra=value", fields.get()));
    CHECK(!io::ParseForm("a=" + std::string(1024, 'x'), fields.get()));
    char small[6] = "old"; CHECK(!io::AppendForm(small, sizeof(small), "key", "long")); CHECK(!std::strcmp(small, "old"));
    std::string screenshotBuffer(65536 + 1024, '\0');
    CHECK(io::AppendForm(screenshotBuffer.data(), unsigned(screenshotBuffer.size()), "command", "submitshot"));
    CHECK(!std::strcmp(screenshotBuffer.data(), "command=submitshot")); // large upload buffer, small encoded fields
    char unterminated[4] = {'x', 'x', 'x', 'x'}; CHECK(!io::AppendForm(unterminated, sizeof(unterminated), "a", "b"));
    CHECK(!io::AppendForm(body, sizeof(body), "", "b")); CHECK(!io::AppendForm(body, sizeof(body), "a", std::string_view("a\0b", 3)));
    CHECK(io::Headers("")); CHECK(io::Headers("Content-Type: application/x-www-form-urlencoded\r\nAuthorization: configured-token\r\n"));
    for (const char* malformed : {"Header", "Header: x\nInjected: x", "Content-Length: 12", "Host: external", "Transfer-Encoding: chunked", "X: a\r\nX: b", "X: a\r\nx: b", " X: a", "X: a\r\n\r\nGET /"}) CHECK(!io::Headers(malformed));
    CHECK(!io::Headers(std::string(io::MaxHeaders + 1, 'x')));
    CHECK(io::Method("GET") && io::Method("POST") && io::Method("HEAD") && io::Method("DELETE"));
    CHECK(!io::Method("get") && !io::Method("GET\r\n") && !io::Method(""));
    for (const char* good : {"test.txt", "folder/file name.txt", "main/iw_00.iwd", "plugins/example.cfg"}) CHECK(io::RelativePath(good));
    for (const char* bad : {"", "../file", "/file", "folder//file", "C:/file", "folder\\file", "folder/", "file.", "file ", "CON.txt", "nul", "COM9.dat", "f?a", "file\n"}) CHECK(!io::RelativePath(bad));
    std::puts("plugin I/O form/header/path policy tests passed");
}
