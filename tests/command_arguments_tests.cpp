#include <command_arguments.hpp>
#include <cstdio>
#include <string>

#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "command argument check failed line %d\n", __LINE__); return 1; } } while (false)
int main() {
    unsigned checks = 0;
    khz::command::Arguments captured;
    const char* values[] = {"command", "http://127.0.0.1:65535/exact?x=1", "", "literal \"quotes\"", "tail\\", "one; quit", "// comment /* literal */"};
    CHECK(captured.Capture(7, values));
    for (unsigned i = 0; i != 7; ++i) CHECK(captured.At(i) == values[i]);
    auto copy = captured;
    captured.text.fill('x');
    for (unsigned i = 0; i != 7; ++i) CHECK(copy.At(i) == values[i]);
    CHECK(!copy.Capture(0, values));
    CHECK(!copy.Capture(513, values));
    CHECK(!copy.Capture(1, nullptr));
    const char* emptyName[] = {""}; CHECK(!copy.Capture(1, emptyName));
    const char* separatorName[] = {"command;quit"}; CHECK(!copy.Capture(1, separatorName));
    const char* nullArgument[] = {"command", nullptr}; CHECK(!copy.Capture(2, nullArgument));
    const char* boundary[] = {"x", "12345"};
    CHECK(!copy.Capture(2, boundary, 7));
    CHECK(copy.At(1) == values[1]); // Failed capture is atomic.
    CHECK(copy.Capture(2, boundary, 8));
    CHECK(copy.bytes == 8);
    std::string excessive(8190, 'x'); const char* oversize[] = {"x", excessive.c_str()};
    CHECK(!copy.Capture(2, oversize));
    CHECK(copy.At(1) == "12345");
    const char* maximum[khz::command::MaxArguments];
    for (auto& value : maximum) value = "";
    maximum[0] = "x";
    CHECK(copy.Capture(khz::command::MaxArguments, maximum));
    CHECK(copy.count == khz::command::MaxArguments);
    std::printf("command argument cases=%u failures=0\n", checks);
    return 0;
}
