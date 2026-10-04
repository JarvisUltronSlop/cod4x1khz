#pragma once
#include <cstring>
struct Material;
// Binary console text after the caret: flip, width, height, native material.
constexpr int CONTXTCMD_LEN_HUDICON = 3 + sizeof(Material *);
constexpr int CONTXTCMD_ARG_HUDICON_MATERIAL = 3;
inline Material *R_ReadHudIconMaterial(const char *payload)
{
    Material *material;
    std::memcpy(&material, payload + CONTXTCMD_ARG_HUDICON_MATERIAL, sizeof(material));
    return material;
}
