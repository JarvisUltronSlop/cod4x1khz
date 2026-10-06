#pragma once
struct usercmd_s;
struct gentity_s;
void KHZ_BenchmarkSceneInit();
bool KHZ_BenchmarkSceneInput(usercmd_s* command);
// Only the explicitly placed diagnostic human is immune to external pushes.
// Normal gameplay and all bot-to-bot interactions retain their stock rules.
bool KHZ_BenchmarkSceneProtectedHuman(const gentity_s* entity);
const char* KHZ_BenchmarkModelsJSON();
