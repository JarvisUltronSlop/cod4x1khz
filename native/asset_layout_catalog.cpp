#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <DbgHelp.h>
#include <cvconst.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct Field { std::string name; DWORD type{}, offset{}, bits{}, length{}; bool bitfield{}; };
struct Type {
    DWORD id{}, tag{}, target{}, count{}, kind{}, base{};
    ULONG64 bytes{};
    std::string name;
    std::vector<Field> fields;
};
struct Catalog {
    HANDLE process = GetCurrentProcess();
    DWORD64 module{};
    std::map<DWORD, Type> types;
    std::vector<std::pair<std::string, DWORD>> globals;
    std::vector<DWORD> pending;
    std::set<DWORD> queued;
    void add(DWORD type) { if (type && queued.insert(type).second) pending.push_back(type); }
    template<class T> bool get(DWORD id, IMAGEHLP_SYMBOL_TYPE_INFO info, T& value) {
        return SymGetTypeInfo(process, module, id, info, &value) != FALSE;
    }
    std::string name(DWORD id) {
        wchar_t* wide{};
        if (!get(id, TI_GET_SYMNAME, wide)) return {};
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
        std::string text(size > 0 ? size : 0, '\0');
        if (size > 0) { WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), size, nullptr, nullptr); text.pop_back(); }
        LocalFree(wide);
        return text;
    }
    void collect() {
        for (std::size_t at = 0; at < pending.size(); ++at) {
            const auto id = pending[at];
            Type type; type.id = id;
            if (!get(id, TI_GET_SYMTAG, type.tag)) throw std::runtime_error("Missing PDB type tag");
            type.name = name(id);
            get(id, TI_GET_LENGTH, type.bytes);
            switch (type.tag) {
            case SymTagPointerType: case SymTagArrayType: case SymTagTypedef: case SymTagEnum:
                if (get(id, TI_GET_TYPEID, type.target)) add(type.target);
                get(id, TI_GET_COUNT, type.count);
                break;
            case SymTagBaseType: get(id, TI_GET_BASETYPE, type.base); break;
            case SymTagUDT: {
                if (!get(id, TI_GET_UDTKIND, type.kind)) throw std::runtime_error("Missing PDB record kind");
                DWORD count{};
                if (!get(id, TI_GET_CHILDRENCOUNT, count)) throw std::runtime_error("Missing PDB member count");
                std::vector<DWORD> storage(count + 2);
                auto* children = reinterpret_cast<TI_FINDCHILDREN_PARAMS*>(storage.data());
                children->Count = count; children->Start = 0;
                if (count && !get(id, TI_FINDCHILDREN, *children)) throw std::runtime_error("Missing PDB members");
                for (DWORD i = 0; i < count; ++i) {
                    const auto child = children->ChildId[i];
                    DWORD tag{}, dataKind{};
                    if (!get(child, TI_GET_SYMTAG, tag) || tag != SymTagData ||
                        !get(child, TI_GET_DATAKIND, dataKind) || dataKind != DataIsMember) continue;
                    Field field; field.name = name(child);
                    if (!get(child, TI_GET_TYPEID, field.type) || !get(child, TI_GET_OFFSET, field.offset))
                        throw std::runtime_error("Missing PDB field type or offset");
                    field.bitfield = get(child, TI_GET_BITPOSITION, field.bits);
                    if (field.bitfield) { ULONG64 length{}; if (get(child, TI_GET_LENGTH, length)) field.length = DWORD(length); }
                    type.fields.push_back(field); add(field.type);
                }
                break;
            }
            }
            types.emplace(id, std::move(type));
        }
    }
};
BOOL CALLBACK enumerate(PSYMBOL_INFO symbol, ULONG, void* data) {
    static_cast<Catalog*>(data)->add(symbol->TypeIndex);
    return TRUE;
}
BOOL CALLBACK enumerateGlobals(PSYMBOL_INFO symbol, ULONG, void* data) {
    if (symbol->Tag != SymTagData) return TRUE;
    const std::string_view name{symbol->Name};
    if (!name.starts_with("var") && !name.starts_with("khz_layout_")) return TRUE;
    auto& catalog = *static_cast<Catalog*>(data);
    catalog.globals.emplace_back(symbol->Name, symbol->TypeIndex);
    catalog.add(symbol->TypeIndex);
    return TRUE;
}
std::string quoted(const std::string& value) {
    std::ostringstream result; result << '"';
    for (const auto byte : value) {
        if (byte == '"' || byte == '\\') result << '\\' << byte;
        else if (static_cast<unsigned char>(byte) < 32) {
            char escape[8]{}; std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned char>(byte)); result << escape;
        } else result << byte;
    }
    result << '"'; return result.str();
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) { std::fputs("asset_layout_catalog <exe or pdb> <output.json>\n", stderr); return 2; }
    Catalog catalog;
    try {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS);
        const auto directory = std::filesystem::absolute(argv[1]).parent_path();
        if (!SymInitializeW(catalog.process, directory.c_str(), FALSE)) throw std::runtime_error("Cannot initialize PDB reader");
        catalog.module = SymLoadModuleExW(catalog.process, nullptr, argv[1], nullptr, 0x10000000, 0, nullptr, 0);
        if (!catalog.module) throw std::runtime_error("Cannot load symbols");
        if (!SymEnumTypes(catalog.process, catalog.module, enumerate, &catalog)) throw std::runtime_error("Cannot enumerate PDB types");
        if (!SymEnumSymbols(catalog.process, catalog.module, "*", enumerateGlobals, &catalog)) throw std::runtime_error("Cannot enumerate loader globals");
        catalog.collect();
        std::ofstream out{std::filesystem::path(argv[2])};
        out << "{\"schema\":1,\"source\":" << quoted(std::filesystem::path(argv[1]).string()) << ",\"types\":[";
        bool first = true;
        for (const auto& [id, type] : catalog.types) {
            if (!first) out << ','; first = false;
            out << "{\"id\":" << id << ",\"tag\":" << type.tag << ",\"name\":" << quoted(type.name)
                << ",\"bytes\":" << type.bytes << ",\"target\":" << type.target << ",\"count\":" << type.count
                << ",\"kind\":" << type.kind << ",\"base\":" << type.base << ",\"fields\":[";
            bool firstField = true;
            for (const auto& field : type.fields) {
                if (!firstField) out << ','; firstField = false;
                out << "{\"name\":" << quoted(field.name) << ",\"type\":" << field.type << ",\"offset\":" << field.offset
                    << ",\"bitfield\":" << (field.bitfield ? "true" : "false") << ",\"bit_position\":" << field.bits
                    << ",\"bit_length\":" << field.length << '}';
            }
            out << "]}";
        }
        out << "],\"globals\":[";
        first = true;
        for (const auto& [name, id] : catalog.globals) {
            if (!first) out << ','; first = false;
            out << "{\"name\":" << quoted(name) << ",\"type\":" << id << '}';
        }
        out << "]}\n"; out.close();
        if (!out) throw std::runtime_error("Cannot write PDB catalog");
        std::printf("Cataloged %zu compiler types\n", catalog.types.size());
        SymCleanup(catalog.process); return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Asset layout catalog failed: %s (Windows error %lu)\n", error.what(), GetLastError());
        SymCleanup(catalog.process); return 1;
    }
}
