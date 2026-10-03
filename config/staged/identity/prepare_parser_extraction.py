"""Prepare a standalone source-extracted proposed parser/wrapper test.

This is implementation validation, not a loaded-game SDK proof. Its actual
helper definitions come from the guarded proposed source, and its literal case
expectations are generated independently from the reviewed field layout.
"""
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def between(text, begin, end):
    first = text.index(begin)
    stop = text.index(end, first)
    return text[first:stop]


def function(text, begin):
    first = text.index(begin)
    cursor = text.index('{', first)
    depth = 1
    cursor += 1
    while depth:
        if text[cursor] == '{': depth += 1
        elif text[cursor] == '}': depth -= 1
        cursor += 1
    return text[first:cursor]


def main():
    proposed = HERE / 'proposal'
    identity = (proposed / 'engine/src/khz/khz_identity.cpp').read_text()
    wrapper = (proposed / 'engine/src/khz/khz_plugins.cpp').read_text()
    contracts = (ROOT / 'sdk/tests/native_identity_contract_probe.inc').read_text()
    output = ROOT / 'build/staged-identity'
    output.mkdir(parents=True, exist_ok=True)
    source = '''#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <cstdio>
#include <cstdarg>
#include <limits>
namespace khz::identity {
namespace {
'''
    source += between(identity, 'std::string_view trim(', 'constexpr std::uint8_t guidSalt')
    source += '\n}\n' + between(identity, 'std::uint64_t StringToSteamId(', 'bool SteamIdToString(') + '\n}\n'
    source += '#define KHZ_API extern "C"\nusing uint64_t = std::uint64_t;\n'
    source += function(wrapper, 'KHZ_API uint64_t __cdecl Plugin_StringToSteamID(') + '\n'
    source += '''static unsigned staging_failures = 0;
static void Plugin_PrintError(const char* format, ...) {
    ++staging_failures;
    va_list args; va_start(args,format); vprintf(format,args); va_end(args);
}
static void Plugin_Printf(const char* format, ...) {
    va_list args; va_start(args,format); vprintf(format,args); va_end(args);
}
'''
    source += between(contracts, 'struct identity_contract_counts', 'static void identity_classify_pair')
    source += (proposed / 'sdk/tests/native_identity_parser_probe.inc').read_text()
    source += '\nint main() { identity_parse_probe(); return staging_failures ? 1 : 0; }\n'
    (output / 'parser_extraction.cpp').write_text(source, newline='\n', encoding='utf8')
    print(output / 'parser_extraction.cpp')


if __name__ == '__main__':
    main()
