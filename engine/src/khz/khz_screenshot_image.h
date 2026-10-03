#pragma once
#include "khz_screenshot_protocol.h"
#include <vector>
namespace khz::screenshot {
// Private service worker only; Windows WIC COM state is local to each call.
bool EncodeJpeg(const std::uint8_t* packedBgr,unsigned width,unsigned height,
    std::vector<std::uint8_t>* result,unsigned* encodedWidth,unsigned* encodedHeight);
bool ValidateJpeg(const std::uint8_t* bytes,unsigned length,unsigned width,unsigned height);
}
