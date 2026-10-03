#ifdef COD4X1KHZ
#include <khz/khz_render_profile.h>
#endif
#include <universal/q_shared.h>
#include "khz_ui.h"
#include "khz_host.h"
#include "khz_profile.h"
#include "khz_controls.h"
#include "khz_admin.h"
#include <client_mp/client_mp.h>
#include <qcommon/cmd.h>
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>
#include <ui/ui_shared.h>
#include <database/database.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#if defined(KISAK_MP) && !defined(KISAK_DEDICATED)
#include <ui_mp/ui_mp.h>

namespace {
constexpr unsigned MaxMenus = 16;
constexpr unsigned MaxItems = 256;
constexpr unsigned AddedItems = 4;
constexpr unsigned ModeMask = 127;
std::uint64_t blockedSingleplayerRequests = 0;
const char* const modeMenus[] = {
    "settings_quick_war", "settings_quick_sd", "settings_quick_sab",
    "settings_quick_dom", "settings_quick_ctf", "settings_quick_dm", "settings_quick_koth"
};
const char* const botCounts[] = { "0", "1", "2", "3", "4", "5", "6", "7",
    "8", "9", "10", "11", "12", "13", "14", "15" };
const char* const difficulties[] = { "Recruit", "Regular", "Hardened", "Veteran" };

union OwnedTypeData {
    listBoxDef_s list;
    editFieldDef_s edit;
    multiDef_s multi;
};
struct OwnedMenu {
    const menuDef_t* original;
    menuDef_t menu;
    itemDef_s items[MaxItems + AddedItems];
    itemDef_s* pointers[MaxItems + AddedItems];
    OwnedTypeData data[MaxItems + AddedItems];
};
struct UIStorage {
    UiContext* frontend;
    unsigned generation;
    unsigned count;
    unsigned modes;
    unsigned disabledSP;
    unsigned addedRows;
    unsigned failures;
    OwnedMenu menus[MaxMenus];
};
// Bounded owned menu records. No UI_Alloc/hunk allocations, and no mutation of
// fastfile database assets or other UiContexts such as the cgame HUD.
UIStorage storage;

bool equal(const char* a, const char* b) { return a && b && !I_stricmp(a, b); }
bool contains(const char* haystack, const char* needle) {
    if (!haystack) return false;
    const auto length = std::strlen(needle);
    for (; *haystack; ++haystack) if (!I_strnicmp(haystack, needle, static_cast<int>(length))) return true;
    return false;
}
void diagnosticString(const char* source, char* destination, unsigned capacity) {
    unsigned used = 0;
    for (unsigned read = 0; source && source[read] && read < 256 && used + 6 < capacity; ++read) {
        const unsigned char ch = source[read];
        if (ch == '"' || ch == '\\') { destination[used++] = '\\'; destination[used++] = ch; }
        else if (ch < 32 || ch > 126) used += std::snprintf(destination + used, capacity - used, "\\u%04x", ch);
        else destination[used++] = ch;
    }
    destination[used] = 0;
}
int modeIndex(const char* name) {
    for (unsigned i = 0; i < ARRAY_COUNT(modeMenus); ++i) if (equal(name, modeMenus[i])) return static_cast<int>(i);
    return -1;
}
bool selected(const menuDef_t* menu) {
    return modeIndex(menu->window.name) >= 0 || equal(menu->window.name, "main") ||
        equal(menu->window.name, "main_text") || equal(menu->window.name, "single_popmenu");
}
void failure(const char* menu, const char* reason) {
    ++storage.failures;
    Com_PrintError(CON_CHANNEL_UI, "cod4x1khz menu '%s': %s\n", menu ? menu : "unnamed", reason);
}

bool copyTypeData(itemDef_s& target, OwnedTypeData& data, const itemDef_s& source) {
    if (!source.typeData.data) return true;
    switch (source.type) {
    case 6: data.list = *source.typeData.listBox; target.typeData.listBox = &data.list; return true;
    case 12: data.multi = *source.typeData.multi; target.typeData.multi = &data.multi; return true;
    case 4: case 9: case 16: case 18: case 11: case 14: case 10: case 0: case 17:
        data.edit = *source.typeData.editField; target.typeData.editField = &data.edit; return true;
    case 13: return true; // Immutable enum dvar name; this union member is a string.
    default: return false;
    }
}
OwnedMenu* clone(menuDef_t* original) {
    if (original->itemCount < 0 || original->itemCount > static_cast<int>(MaxItems) ||
        (original->itemCount && !original->items) || storage.count >= MaxMenus) {
        failure(original->window.name, "bounded menu capacity exceeded"); return nullptr;
    }
    auto& owned = storage.menus[storage.count];
    owned.original = original;
    owned.menu = *original;
    owned.menu.items = owned.pointers;
    for (int i = 0; i < original->itemCount; ++i) {
        if (!original->items[i]) { failure(original->window.name, "null item record"); return nullptr; }
        owned.items[i] = *original->items[i];
        owned.items[i].parent = &owned.menu;
        if (!copyTypeData(owned.items[i], owned.data[i], *original->items[i])) {
            failure(original->window.name, "unsupported mutable item data"); return nullptr;
        }
        owned.pointers[i] = &owned.items[i];
    }
    ++storage.count;
    return &owned;
}
void disableSP(OwnedMenu& owned) {
    for (int i = 0; i < owned.menu.itemCount; ++i) {
        auto& item = owned.items[i];
        if (equal(item.text, "@MENU_SINGLE_PLAYER") || contains(item.action, "single_popmenu") ||
            contains(item.action, "startsingleplayer") || contains(item.onAccept, "startsingleplayer")) {
            item.dvarTest = "khz_singleplayer_available";
            item.enableDvar = "1";
            item.dvarFlags = (item.dvarFlags & ~3) | 1;
            // The stock MP main menu's disabled colour is transparent because
            // vanilla never disables this button. Keep the row visibly grey.
            if (owned.menu.disableColor[3] <= 0.1f) {
                owned.menu.disableColor[0] = owned.menu.disableColor[1] = owned.menu.disableColor[2] = 0.45f;
                owned.menu.disableColor[3] = 1.0f;
            }
            ++storage.disabledSP;
        }
    }
}
void clearExpressions(itemDef_s& item) {
    item.visibleExp = {}; item.textExp = {}; item.materialExp = {};
    item.rectXExp = {}; item.rectYExp = {}; item.rectWExp = {}; item.rectHExp = {}; item.forecolorAExp = {};
}
void newRowItem(itemDef_s& item, const itemDef_s& stock, menuDef_t& parent, const char* name, float y) {
    item = stock;
    item.parent = &parent;
    item.window.name = name;
    item.window.group = "khz_bot_options";
    item.window.rectClient.y = y;
    item.window.dynamicFlags[0] = 4; // Visible, with no inherited focus/mouse state.
    item.window.nextTime = 0;
    item.textRect[0] = {};
    item.cursorPos[0] = 0;
    item.mouseEnterText = nullptr; item.mouseExitText = nullptr;
    item.mouseEnter = nullptr; item.mouseExit = nullptr;
    item.action = nullptr; item.onAccept = nullptr; item.onFocus = nullptr;
    item.leaveFocus = nullptr; item.onKey = nullptr;
    item.dvar = nullptr; item.dvarTest = nullptr; item.enableDvar = nullptr; item.dvarFlags = 0;
    clearExpressions(item);
}
bool addBotRows(OwnedMenu& owned, int mode) {
    itemDef_s* stockControl = nullptr;
    itemDef_s* stockLabel = nullptr;
    float bottom = 0;
    float rowStep = 24;
    // Select the stock rules column semantically: a bound multi-select, using
    // the leftmost column. Rects/styles come from loaded assets, not item indices.
    for (int i = 0; i < owned.menu.itemCount; ++i) {
        auto* item = owned.pointers[i];
        if (item->type != 12 || !item->dvar || !item->typeData.multi || item->window.rectClient.y < 40 ||
            item->window.rectClient.w < 200 || item->window.rectClient.h < 10 || item->window.rectClient.h > 40) continue;
        if (!stockControl || item->window.rectClient.x < stockControl->window.rectClient.x ||
            (item->window.rectClient.x == stockControl->window.rectClient.x &&
             item->window.rectClient.y < stockControl->window.rectClient.y)) stockControl = item;
    }
    if (!stockControl) { failure(owned.menu.window.name, "stock multi-select row unavailable"); return false; }
    const auto rect = stockControl->window.rectClient;
    bottom = rect.y;
    for (int i = 0; i < owned.menu.itemCount; ++i) {
        auto* item = owned.pointers[i];
        const auto& r = item->window.rectClient;
        if (item->type == 12 && item->dvar && r.x == rect.x && r.horzAlign == rect.horzAlign && r.vertAlign == rect.vertAlign) {
            bottom = (std::max)(bottom, r.y);
            const float delta = r.y - rect.y;
            if (delta >= rect.h && delta < rowStep) rowStep = delta;
        }
        if (item->type == 1 && r.x == rect.x && r.y == rect.y && r.h == rect.h && r.w <= rect.w &&
            (item->text || item->textExp.numEntries)) stockLabel = item;
    }
    if (!stockLabel || !std::isfinite(bottom) || bottom + rowStep * 2 + rect.h > 420 ||
        owned.menu.itemCount + 4 > static_cast<int>(MaxItems + AddedItems)) {
        failure(owned.menu.window.name, "stock row layout has insufficient room"); return false;
    }
    const char* const names[] = { "khz_bot_count_label", "khz_bot_count", "khz_bot_difficulty_label", "khz_bot_difficulty" };
    const char* const labels[] = { "Bot Count", "Bot Difficulty" };
    const char* const dvars[] = { "ui_khz_botCount", "ui_khz_botDifficulty" };
    for (unsigned row = 0; row < 2; ++row) {
        const int labelIndex = owned.menu.itemCount++;
        const int controlIndex = owned.menu.itemCount++;
        auto& label = owned.items[labelIndex]; auto& control = owned.items[controlIndex];
        const float y = bottom + rowStep * static_cast<float>(row + 1);
        newRowItem(label, *stockLabel, owned.menu, names[row * 2], y);
        label.text = labels[row]; label.typeData.data = nullptr;
        label.window.staticFlags |= 0x100000; // Stock decoration/nonfocusable label.
        newRowItem(control, *stockControl, owned.menu, names[row * 2 + 1], y);
        control.text = nullptr; control.dvar = dvars[row];
        control.window.staticFlags &= ~0x100000;
        control.action = "\"play\" \"mouse_click\" ; ";
        control.onFocus = "\"play\" \"mouse_over\" ; ";
        control.type = control.dataType = 12;
        auto& choices = owned.data[controlIndex].multi;
        choices = {};
        choices.count = row ? 4 : 16;
        choices.strDef = 0;
        for (int choice = 0; choice < choices.count; ++choice) {
            choices.dvarList[choice] = row ? difficulties[choice] : botCounts[choice];
            choices.dvarValue[choice] = static_cast<float>(choice);
        }
        control.typeData.multi = &choices;
        owned.pointers[labelIndex] = &label; owned.pointers[controlIndex] = &control;
    }
    storage.addedRows += 2;
    storage.modes |= 1u << mode;
    Menu_UpdatePosition(storage.frontend->localClientNum, &owned.menu);
    return true;
}
}

void KHZ_UIBegin(UiContext* frontend) {
    iassert(Sys_IsMainThread());
    storage.frontend = frontend;
    ++storage.generation;
    storage.count = storage.modes = storage.disabledSP = storage.addedRows = storage.failures = 0;
    Dvar_RegisterBool("khz_singleplayer_available", false, DVAR_ROM, "Single-player is not part of the multiplayer client");
    Dvar_RegisterInt("ui_khz_botCount", 15, 0, 15, DVAR_ARCHIVE, "Desired bots, yielding slots to joining humans");
    Dvar_RegisterInt("ui_khz_botDifficulty", 1, 0, 3, DVAR_ARCHIVE, "Recruit, Regular, Hardened or Veteran");
}
void KHZ_UIEnd(UiContext* frontend) {
    iassert(Sys_IsMainThread());
    if (storage.frontend == frontend) storage.frontend = nullptr;
}
menuDef_t* KHZ_UIAdaptMenu(UiContext* context, menuDef_t* original) {
    iassert(Sys_IsMainThread());
    if (context != storage.frontend || !original || !IsFastFileLoad() || !selected(original)) return original;
    for (unsigned i = 0; i < storage.count; ++i) if (storage.menus[i].original == original) return &storage.menus[i].menu;
    auto* owned = clone(original);
    if (!owned) return original;
    disableSP(*owned);
    const int mode = modeIndex(original->window.name);
    if (mode >= 0) addBotRows(*owned, mode);
    return &owned->menu;
}
bool KHZ_UIRunMenuScript(UiContext* context, const char* verb, const char**) {
    iassert(Sys_IsMainThread());
    if (context != storage.frontend || !verb) return false;
    if (equal(verb, "startSingleplayer")) {
        KHZ_UIRecordSingleplayerBlock();
        Com_Printf(CON_CHANNEL_UI, "Single-player is unavailable in cod4x1khz.\n"); return true;
    }
    if (equal(verb, "Leave")) KHZ_StopOwnedHost(true);
    if (!equal(verb, "StartServer")) return false;
    const int mapIndex = Dvar_GetInt("ui_currentNetMap");
    const int typeIndex = Dvar_GetInt("ui_netGametype");
    if (mapIndex < 0 || mapIndex >= sharedUiInfo.mapCount || mapIndex >= static_cast<int>(ARRAY_COUNT(sharedUiInfo.mapList)) ||
        typeIndex < 0 || typeIndex >= sharedUiInfo.numGameTypes || typeIndex >= static_cast<int>(ARRAY_COUNT(sharedUiInfo.gameTypes))) {
        failure("createserver", "invalid map or game-mode selection");
        Dvar_SetStringByName("com_errorMessage", "Select a valid map and game mode before starting a match.");
        UI_SetActiveMenu(context->localClientNum, UIMENU_MAIN); return true;
    }
    KHZ_StartLocalHost(context->localClientNum, sharedUiInfo.mapList[mapIndex].mapLoadName,
        sharedUiInfo.gameTypes[typeIndex].gameType, Dvar_GetInt("ui_dedicated"));
    return true;
}
const char* KHZ_UIStatusJSON() {
    static char result[640];
    std::snprintf(result, sizeof(result), "{\"generation\":%u,\"active\":%s,\"adapted_menus\":%u,\"disabled_sp_items\":%u,"
        "\"bot_rows\":%u,\"mode_mask\":%u,\"required_mode_mask\":%u,\"failures\":%u,\"bot_count\":%d,\"bot_difficulty\":%d,\"blocked_singleplayer_requests\":%llu}",
        storage.generation, storage.frontend ? "true" : "false", storage.count, storage.disabledSP,
        storage.addedRows, storage.modes, ModeMask, storage.failures, Dvar_GetInt("ui_khz_botCount"), Dvar_GetInt("ui_khz_botDifficulty"), blockedSingleplayerRequests);
    return result;
}
void KHZ_UIRecordSingleplayerBlock() { iassert(Sys_IsMainThread()); ++blockedSingleplayerRequests; }
const char* KHZ_UIProbe(const char* action) {
    static char result[16384];
    iassert(Sys_IsMainThread());
    if (!Dvar_GetBool("khz_test_controls") || !storage.frontend || !action) return "{\"error\":\"frontend test controls unavailable\"}";
    if (!I_strnicmp(action, "renderprofile ", 14)) return KHZ_RenderProfileProbe(action + 14);
    KHZ_CommandControlScope controls;
    if (!I_strnicmp(action, "inspect ", 8) || !I_strnicmp(action, "details ", 8) || !I_strnicmp(action, "activate ", 9) || !I_strnicmp(action, "key ", 4)) {
        const bool inspect = !I_strnicmp(action, "inspect ", 8);
        const bool details = !I_strnicmp(action, "details ", 8);
        const bool activate = !I_strnicmp(action, "activate ", 9);
        char name[96]{}; int index = 0, key = 0;
        const int parsed = std::sscanf(action + (inspect || details ? 8 : activate ? 9 : 4), "%95s %d %d", name, &index, &key);
        menuDef_t* menu = nullptr;
        for (int i = 0; i < storage.frontend->menuCount; ++i) {
            auto* candidate = storage.frontend->Menus[i];
            if (candidate && equal(candidate->window.name, name)) { menu = candidate; break; }
        }
        if (!menu || parsed < 1 || index < 0 || menu->itemCount < 0 || menu->itemCount > 512 || !menu->items)
            return "{\"result\":\"invalid stock menu request\"}";
        if (details) {
            if (parsed != 2 || index >= menu->itemCount || !menu->items[index]) return "{\"result\":\"invalid stock item\"}";
            const auto* item = menu->items[index];
            const auto& expression = item->visibleExp;
            if (expression.numEntries < 0 || expression.numEntries > 128 || (expression.numEntries && !expression.entries))
                return "{\"result\":\"stock expression exceeds diagnostic bound\"}";
            char actionText[1024], focus[1024], onOpen[1024];
            diagnosticString(item->action, actionText, sizeof(actionText));
            diagnosticString(item->onFocus, focus, sizeof(focus));
            diagnosticString(menu->onOpen, onOpen, sizeof(onOpen));
            int used = std::snprintf(result, sizeof(result), "{\"result\":\"stock item details\",\"action\":\"%s\",\"on_focus\":\"%s\",\"on_open\":\"%s\",\"visible_expression\":[", actionText, focus, onOpen);
            for (int i = 0; i < expression.numEntries && used + 1024 < sizeof(result); ++i) {
                const auto* entry = expression.entries[i];
                if (!entry) return "{\"result\":\"invalid stock expression entry\"}";
                if (!entry->type) {
                    used += std::snprintf(result + used, sizeof(result) - used, "%s{\"operator\":%d}", i ? "," : "", static_cast<int>(entry->data.op));
                } else if (entry->data.operand.dataType == VAL_STRING) {
                    char value[512]; diagnosticString(entry->data.operand.internals.string, value, sizeof(value));
                    used += std::snprintf(result + used, sizeof(result) - used, "%s{\"string\":\"%s\"}", i ? "," : "", value);
                } else if (entry->data.operand.dataType == VAL_INT) {
                    used += std::snprintf(result + used, sizeof(result) - used, "%s{\"integer\":%d}", i ? "," : "", entry->data.operand.internals.intVal);
                } else if (entry->data.operand.dataType == VAL_FLOAT && std::isfinite(entry->data.operand.internals.floatVal)) {
                    used += std::snprintf(result + used, sizeof(result) - used, "%s{\"float\":%.9g}", i ? "," : "", entry->data.operand.internals.floatVal);
                } else return "{\"result\":\"invalid stock expression operand\"}";
            }
            std::snprintf(result + used, sizeof(result) - used, "]}"); return result;
        }
        if (!inspect) {
            if (parsed < 2 || index >= menu->itemCount || !menu->items[index]) return "{\"result\":\"invalid stock item\"}";
            auto* item = menu->items[index];
            if ((item->window.staticFlags & 0x100000) || !Item_IsVisible(0, item) ||
                ((item->dvarFlags & 3) && !Item_EnableShowViaDvar(item, 1))) return "{\"result\":\"stock item disabled\"}";
            if (activate) Item_Action(storage.frontend, item);
            else {
                if (parsed != 3 || (key != K_LEFTARROW && key != K_RIGHTARROW && key != K_ENTER)) return "{\"result\":\"invalid stock control key\"}";
                if (item->parent != menu || Menu_GetFocused(storage.frontend) != menu ||
                    !Item_SetFocus(storage.frontend, item, item->window.rect.x, item->window.rect.y) ||
                    !Window_HasFocus(storage.frontend->localClientNum, &item->window))
                    return "{\"result\":\"stock control focus unavailable\"}";
                // Follow the native dispatcher, including handled-key actions
                // and the keybinding/edit-field state. A returned dispatch is
                // not proof of a changed setting; fixtures inspect the actual
                // dvar/archive and restart generation independently.
                Menu_HandleKey(storage.frontend, menu, key, 1);
                return "{\"result\":\"stock key dispatched\"}";
            }
            return "{\"result\":\"stock action executed\"}";
        }
        int used = std::snprintf(result, sizeof(result), "{\"result\":\"inspected\",\"menu\":\"%s\",\"item_count\":%d,\"offset\":%d,\"items\":[", name, menu->itemCount, index);
        bool first = true;
        for (int i = index; i < menu->itemCount && i < index + 16 && used + 2000 < sizeof(result); ++i) {
            auto* item = menu->items[i]; if (!item) continue;
            char itemName[512], text[512], dvar[512];
            const char* displayedText = item->text;
            if (!displayedText && item->textExp.numEntries)
                displayedText = GetExpressionResultString(storage.frontend->localClientNum, &item->textExp);
            diagnosticString(item->window.name, itemName, sizeof(itemName)); diagnosticString(displayedText, text, sizeof(text)); diagnosticString(item->dvar, dvar, sizeof(dvar));
            used += std::snprintf(result + used, sizeof(result) - used,
                "%s{\"index\":%d,\"name\":\"%s\",\"text\":\"%s\",\"type\":%d,\"dvar\":\"%s\",\"visible\":%s,\"enabled\":%s,\"action\":%s,\"rect\":[%.3f,%.3f,%.3f,%.3f]}",
                first ? "" : ",", i, itemName, text, item->type, dvar, Item_IsVisible(0, item) ? "true" : "false",
                (!(item->dvarFlags & 3) || Item_EnableShowViaDvar(item, 1)) ? "true" : "false", item->action ? "true" : "false",
                item->window.rectClient.x, item->window.rectClient.y, item->window.rectClient.w, item->window.rectClient.h);
            first = false;
        }
        std::snprintf(result + used, sizeof(result) - used, "]}"); return result;
    }
    const char* outcome = "queried";
    if (!I_strnicmp(action, "console ", 8)) {
        char commandName[64];
        bool allowed = false;
        if (khz::admin::ValidateCommand(action + 8, commandName, sizeof(commandName))) {
            if (equal(commandName, "khz_scene") && Dvar_GetBool("khz_benchmark")) allowed = true;
            for (const char* candidate : {"fovscale", "cg_fovScale", "com_maxfps", "r_vsync", "restoreunlocks", "khz_profile_selftest", "khz_test_ads", "khz_stall", "khz_dump_scripts", "screenshot", "vid_restart", "snd_restart"})
                if (equal(commandName, candidate)) allowed = true;
        }
        if (allowed && std::strlen(action + 8) <= 1023) { Cbuf_AddText(0, va("%s\n", action + 8)); outcome = "queued"; }
        else outcome = "diagnostic command rejected";
    } else if (!I_strnicmp(action, "server ", 7)) {
        outcome = KHZ_HostConsoleCommand(action + 7) ? "server command queued" : "server command unavailable";
    } else if (!I_strnicmp(action, "menu ", 5)) {
        const char* name = action + 5;
        menuDef_t* found = nullptr;
        for (int i = 0; i < storage.frontend->menuCount; ++i) {
            auto* candidate = storage.frontend->Menus[i];
            if (candidate && equal(candidate->window.name, name)) { found = candidate; break; }
        }
        if (!found) outcome = "unknown menu";
        else {
            Menus_CloseAll(storage.frontend);
            outcome = Menus_OpenByName(storage.frontend, name) ? "opened" : "open failed";
        }
    } else if (!I_strnicmp(action, "host ", 5)) {
        char map[64], mode[32]; int count = -1, difficulty = -1, consumed = 0;
        if (std::sscanf(action + 5, "%63s %31s %d %d %n", map, mode, &count, &difficulty, &consumed) == 4 &&
            !action[5 + consumed] && count >= 0 && count <= 15 && difficulty >= 0 && difficulty <= 3) {
            int mapIndex = -1, modeIndex = -1;
            for (int i = 0; i < sharedUiInfo.mapCount; ++i) if (equal(sharedUiInfo.mapList[i].mapLoadName, map)) mapIndex = i;
            for (int i = 0; i < sharedUiInfo.numGameTypes; ++i) if (equal(sharedUiInfo.gameTypes[i].gameType, mode)) modeIndex = i;
            if (mapIndex >= 0 && modeIndex >= 0) {
                Dvar_SetIntByName("ui_currentNetMap", mapIndex); Dvar_SetIntByName("ui_netGametype", modeIndex);
                Dvar_SetIntByName("ui_dedicated", 0); Dvar_SetIntByName("ui_khz_botCount", count); Dvar_SetIntByName("ui_khz_botDifficulty", difficulty);
                KHZ_UIRunMenuScript(storage.frontend, "StartServer", nullptr); outcome = "start requested";
            } else outcome = "stock map or mode unavailable";
        } else outcome = "invalid host request";
    } else if (equal(action, "singleplayer") || equal(action, "screenshot") || equal(action, "vid_restart") ||
        equal(action, "snd_restart") || equal(action, "unlockall") || equal(action, "profile_test") || equal(action, "leave") || equal(action, "quit")) {
        const char* command = equal(action, "singleplayer") ? "startSingleplayer" :
            equal(action, "profile_test") ? "khz_profile_selftest" : equal(action, "leave") ? "disconnect" : action;
        Cbuf_AddText(0, va("%s\n", command)); outcome = "queued";
    } else if (!equal(action, "list") && !equal(action, "status")) outcome = "unknown action";
    int used = std::snprintf(result, sizeof(result),
        "{\"result\":\"%s\",\"top_menu\":\"%s\",\"fullscreen\":%s,\"vsync\":%s,\"render_cap\":%d,\"fov_scale\":%.6f,\"ui\":%s,\"host\":%s,\"profile\":%s,\"menus\":[",
        outcome, UI_GetTopActiveMenuName(0) ? UI_GetTopActiveMenuName(0) : "", Dvar_GetBool("r_fullscreen") ? "true" : "false",
        Dvar_GetBool("r_vsync") ? "true" : "false", Dvar_GetInt("com_maxfps"), Dvar_GetFloat("cg_fovScale"),
        KHZ_UIStatusJSON(), KHZ_HostStatusJSON(), KHZ_ProfileStatusJSON());
    if (equal(action, "list")) {
        bool first = true;
        for (int i = 0; i < storage.frontend->menuCount && used < sizeof(result) - 128; ++i) {
            const auto* menu = storage.frontend->Menus[i];
            if (!menu || !menu->window.name) continue;
            bool safe = true;
            for (const char* c = menu->window.name; *c; ++c) if (*c < 32 || *c == '"' || *c == '\\') safe = false;
            if (safe) { used += std::snprintf(result + used, sizeof(result) - used, "%s\"%s\"", first ? "" : ",", menu->window.name); first = false; }
        }
    }
    const bool validTypes = sharedUiInfo.numGameTypes >= 0 &&
        sharedUiInfo.numGameTypes <= static_cast<int>(ARRAY_COUNT(sharedUiInfo.gameTypes));
    used += std::snprintf(result + used, sizeof(result) - used,
        "],\"game_types_valid\":%s,\"game_types\":[", validTypes ? "true" : "false");
    if (equal(action, "list") && validTypes) {
        for (int i = 0; i < sharedUiInfo.numGameTypes && used + 256 < sizeof(result); ++i) {
            char type[128]; diagnosticString(sharedUiInfo.gameTypes[i].gameType, type, sizeof(type));
            used += std::snprintf(result + used, sizeof(result) - used, "%s\"%s\"", i ? "," : "", type);
        }
    }
    std::snprintf(result + used, sizeof(result) - used, "]}");
    return result;
}
#else
void KHZ_UIBegin(UiContext*) {}
void KHZ_UIEnd(UiContext*) {}
menuDef_t* KHZ_UIAdaptMenu(UiContext*, menuDef_t* original) { return original; }
bool KHZ_UIRunMenuScript(UiContext*, const char*, const char**) { return false; }
const char* KHZ_UIStatusJSON() { return "{\"active\":false}"; }
const char* KHZ_UIProbe(const char*) { return "{\"error\":\"dedicated process\"}"; }
void KHZ_UIRecordSingleplayerBlock() {}
#endif
