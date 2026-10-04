#include <universal/q_shared.h>
#include "database.h"
#include "asset_native_stream.h"
#include <asset_bootstrap_layout.hpp>

#if defined(_WIN64)
namespace {
struct NativeDelay {
    std::uint8_t* pointer;
    std::uint32_t bytes, block, offset;
    const khz::asset_wire::Schema* schema;
};
struct NativeReference { uintptr_t* slot; std::uint32_t encoded; bool alias; };
std::vector<NativeDelay> nativeDelays;
std::vector<NativeReference> nativeReferences;

void ResolveReferences(bool final)
{
    for (auto at = nativeReferences.begin(); at != nativeReferences.end();)
    {
        if ((!at->alias || final) && g_assetRelocations.registered(at->encoded))
        {
            const auto value = at->alias ? g_assetRelocations.alias(at->encoded) :
                reinterpret_cast<uintptr_t>(g_assetRelocations.resolve(at->encoded));
            std::memcpy(at->slot, &value, sizeof(value));
            at = nativeReferences.erase(at);
        }
        else ++at;
    }
    if (final && !nativeReferences.empty())
        throw std::runtime_error("Fastfile contains unresolved native asset references");
}

void DecodeRecords(const NativeDelay& read, std::span<const std::byte> wire)
{
    const auto& schema = *read.schema;
    const auto count = read.bytes / schema.wireBytes;
    auto* native = reinterpret_cast<std::byte*>(read.pointer);
    std::vector<khz::asset_wire::Field> fields;
    if (!schema.dynamic)
    {
        const auto layout = schema.select(wire.first(schema.wireBytes), fields);
        khz::asset_wire::decode(layout, wire, {native, std::size_t(count) * schema.nativeBytes}, count);
        g_assetRelocations.add(read.block, read.offset, layout,
            {native, std::size_t(count) * schema.nativeBytes}, count);
    }
    else for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto record = wire.subspan(std::size_t(i) * schema.wireBytes, schema.wireBytes);
        const auto layout = schema.select(record, fields);
        khz::asset_wire::decode(layout, record, {native + std::size_t(i) * schema.nativeBytes, schema.nativeBytes});
        g_assetRelocations.add(read.block, read.offset + i * schema.wireBytes, layout,
            {native + std::size_t(i) * schema.nativeBytes, schema.nativeBytes});
    }
    ResolveReferences(false);
}
void Delay(NativeDelay read)
{
    if (nativeDelays.size() >= ARRAY_COUNT(g_streamDelayArray))
        throw std::runtime_error("Native asset delayed stream capacity exceeded");
    nativeDelays.push_back(read); ++g_streamDelayIndex;
}
void ConvertReference(uintptr_t* data, bool alias)
{
    uintptr_t value{}; std::memcpy(&value, data, sizeof(value));
    if (!value || value >= UINT32_MAX - 1u || value > UINT32_MAX)
        throw std::runtime_error("Invalid encoded native asset reference");
    const auto encoded = static_cast<std::uint32_t>(value);
    if (g_assetRelocations.registered(encoded))
    {
        value = alias ? g_assetRelocations.alias(encoded) :
            reinterpret_cast<uintptr_t>(g_assetRelocations.resolve(encoded));
        std::memcpy(data, &value, sizeof(value));
    }
    else nativeReferences.push_back({data, encoded, alias});
}
}
void DB_ResetNativeLoads() { nativeDelays.clear(); nativeReferences.clear(); }
void DB_FinishNativeReferences() { ResolveReferences(true); }
void DB_CheckNativeTemporaryReferences(const std::uint8_t* first, const std::uint8_t* end)
{
    ResolveReferences(false);
    const auto beginAddress = reinterpret_cast<uintptr_t>(first), endAddress = reinterpret_cast<uintptr_t>(end);
    for (const auto& reference : nativeReferences)
    {
        const auto address = reinterpret_cast<uintptr_t>(reference.slot);
        if (address >= beginAddress && address < endAddress)
            throw std::runtime_error("Temporary asset contains an unresolved native reference before rewind");
    }
}
#endif

void DB_LoadTypedStream(bool atStreamStart, std::uint8_t* pointer, std::int32_t wireBytes,
    const khz::asset_wire::Schema& schema)
{
#if !defined(_WIN64)
    Load_Stream(atStreamStart, pointer, wireBytes);
#else
    iassert(atStreamStart == (pointer == DB_GetStreamPos()));
    if (!atStreamStart || !wireBytes) return;
    if (wireBytes < 0 || wireBytes % schema.wireBytes) {
        char message[256]{};
        std::snprintf(message, sizeof(message), "Asset %s typed read (%d bytes) does not match disk stride %u at block %u offset %u",
            schema.name, wireBytes, schema.wireBytes, g_streamPosIndex, g_assetStreams.wirePosition());
        throw std::runtime_error(message);
    }
    const auto count = static_cast<std::uint32_t>(wireBytes) / schema.wireBytes;
    if (count > g_assetStreams.remaining().size() / schema.nativeBytes)
        throw std::runtime_error("Asset typed read exceeds native stream storage");
    const auto nativeBytes = count * schema.nativeBytes;
    g_assetStreams.check(wireBytes, nativeBytes);
    const NativeDelay read{pointer, static_cast<std::uint32_t>(wireBytes), g_streamPosIndex,
        g_assetStreams.wirePosition(), &schema};
    if (g_streamPosIndex == 2 || g_streamPosIndex == 3)
    {
        std::memset(pointer, 0, nativeBytes); Delay(read);
    }
    else
    {
        std::vector<std::byte> wire(wireBytes);
        if (g_streamPosIndex != 1) DB_LoadXFileData(reinterpret_cast<std::uint8_t*>(wire.data()), wireBytes);
        DecodeRecords(read, wire);
    }
    DB_AdvanceNativeStream(wireBytes, nativeBytes);
#endif
}

void DB_LoadPointerStream(bool atStreamStart, std::uint8_t* pointer, std::int32_t wireBytes)
{
    static constexpr khz::asset_wire::Schema schema{"encoded pointer", 4, sizeof(void*),
        [](std::span<const std::byte>, std::vector<khz::asset_wire::Field>&) { return khz::asset_wire::pointerLayout(); }};
    DB_LoadTypedStream(atStreamStart, pointer, wireBytes, schema);
}


void __cdecl Load_Stream(bool atStreamStart, uint8_t *ptr, int32_t size)
{
    iassert(atStreamStart == (ptr == DB_GetStreamPos()));
    if (atStreamStart && size)
    {
#if defined(_WIN64)
        if (size < 0 || static_cast<std::size_t>(size) > g_assetStreams.remaining().size())
            throw std::runtime_error("Raw asset read exceeds native stream storage");
        g_assetStreams.check(size, size);
        if (g_streamPosIndex == 2 || g_streamPosIndex == 3)
            Delay({ptr, static_cast<std::uint32_t>(size), g_streamPosIndex, g_assetStreams.wirePosition(), nullptr});
        else if (g_streamPosIndex == 1) std::memset(ptr, 0, size);
        else DB_LoadXFileData(ptr, size);
        DB_RegisterNativeBytes(ptr, size);
        ResolveReferences(false);
#else
        if (g_streamPosIndex - 1 < 3)
        {
            if (g_streamPosIndex == 1)
            {
                memset(ptr, 0, size);
            }
            else
            {
                bcassert(g_streamDelayIndex, ARRAY_COUNT(g_streamDelayArray));
                g_streamDelayArray[g_streamDelayIndex].ptr = ptr;
                g_streamDelayArray[g_streamDelayIndex++].size = size;
            }
        }
        else
        {
            DB_LoadXFileData(ptr, size);
        }
#endif
        DB_IncStreamPos(size);
    }
}

void __cdecl Load_DelayStream()
{
#if defined(_WIN64)
    for (const auto& read : nativeDelays)
    {
        if (read.schema)
        {
            std::vector<std::byte> wire(read.bytes);
            DB_LoadXFileData(reinterpret_cast<std::uint8_t*>(wire.data()), read.bytes);
            DecodeRecords(read, wire);
        }
        else DB_LoadXFileData(read.pointer, read.bytes);
    }
    nativeDelays.clear();
#else
    uint32_t index; // [esp+4h] [ebp-8h]

    for (index = 0; index < g_streamDelayIndex; ++index)
        DB_LoadXFileData((unsigned char*)g_streamDelayArray[index].ptr, g_streamDelayArray[index].size);
#endif
}

void __cdecl DB_ConvertOffsetToAlias(uintptr_t *data)
{
#if defined(_WIN64)
    ConvertReference(data, true);
#else
    uint32_t offset; // [esp+0h] [ebp-8h]

    offset = *data;
    iassert((offset && (offset != -1) && (offset != -2)));
    *data = *(uint32_t *)&g_streamZoneMem->blocks[(offset - 1) >> 28].data[(offset - 1) & 0xFFFFFFF];
#endif
}

void __cdecl DB_ConvertOffsetToPointer(uintptr_t *data)
{
#if defined(_WIN64)
    ConvertReference(data, false);
#else
    *data = reinterpret_cast<uintptr_t>(&g_streamZoneMem->blocks[(uint32_t)(*data - 1) >> 28].data[(*data - 1) & 0xFFFFFFF]);
#endif
}

void __cdecl Load_XStringCustom(char **str)
{
    uint8_t *pos; // [esp+0h] [ebp-8h]
    char *s; // [esp+4h] [ebp-4h]

    s = *str;
    for (pos = (uint8_t *)*str; ; ++pos)
    {
#if defined(_WIN64)
        const auto bytes = static_cast<std::size_t>(pos - reinterpret_cast<uint8_t*>(s)) + 1;
        if (bytes > g_assetStreams.remaining().size() || bytes > g_assetStreams.wireRemaining())
            throw std::runtime_error("Asset string exceeds native stream storage");
#endif
        DB_LoadXFileData(pos, 1u);
        if (!*pos)
            break;
    }
#if defined(_WIN64)
    DB_RegisterNativeBytes(reinterpret_cast<uint8_t*>(s), static_cast<uint32_t>(pos - reinterpret_cast<uint8_t*>(s) + 1));
    ResolveReferences(false);
#endif
    DB_IncStreamPos(static_cast<int32_t>(pos - reinterpret_cast<uint8_t*>(s) + 1));
}

void __cdecl Load_TempStringCustom(char **str)
{
    const char * string; // [esp+0h] [ebp-4h]

    Load_XStringCustom(str);
    if (*str)
        // ScriptStringList stores interned 16-bit IDs in pointer-sized slots;
        // Load_ScriptStringCustom reads those IDs rather than dereferencing them.
        string = reinterpret_cast<const char*>(static_cast<uintptr_t>(SL_GetString(*str, 4u)));
    else
        string= 0;
    *str = (char *)string;
}

