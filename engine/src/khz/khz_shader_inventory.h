#pragma once
#include <cstdint>
// Called only during asset loading, before GPU creation can overwrite anything
// in a stock shader record. Bytecode and metadata stay in the private FS home.
void KHZ_CaptureStockShader(const char* name,const std::uint32_t* program,unsigned words,unsigned renderer,const char* stage);
struct MaterialPass;
void KHZ_CaptureStockPass(const MaterialPass* pass);
