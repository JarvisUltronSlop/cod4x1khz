#pragma once
#include <cstdint>
#include <asset_schema.hpp>
#if defined(_WIN64)
#include <asset_stream.hpp>
extern khz::asset_wire::Streams g_assetStreams;
extern khz::asset_wire::Relocations g_assetRelocations;
void DB_AdvanceNativeStream(std::uint32_t wireBytes, std::uint32_t nativeBytes);
void DB_RegisterNativeBytes(std::uint8_t* pointer, std::uint32_t bytes);
void DB_FinishNativeReferences();
void DB_ResetNativeLoads();
void DB_CheckNativeTemporaryReferences(const std::uint8_t* first, const std::uint8_t* end);
#endif
void DB_LoadTypedStream(bool atStreamStart, std::uint8_t* pointer, std::int32_t wireBytes,
    const khz::asset_wire::Schema& schema);
void DB_LoadPointerStream(bool atStreamStart, std::uint8_t* pointer, std::int32_t wireBytes);
