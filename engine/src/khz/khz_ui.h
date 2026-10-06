#pragma once

struct UiContext;
struct menuDef_t;

// Frontend UI storage is independent of the cgame HUD UiContext. All calls
// belong to the main frontend owner and to a quiescent UI asset generation.
void KHZ_UIBegin(UiContext* frontend);
void KHZ_UIEnd(UiContext* frontend);
menuDef_t* KHZ_UIAdaptMenu(UiContext* context, menuDef_t* original);
bool KHZ_UIOpenPauseMenu(UiContext* frontend);
bool KHZ_UIRunMenuScript(UiContext* context, const char* verb, const char** args);
const char* KHZ_UIStatusJSON();
const char* KHZ_UIProbe(const char* action);
void KHZ_UIRecordSingleplayerBlock();
