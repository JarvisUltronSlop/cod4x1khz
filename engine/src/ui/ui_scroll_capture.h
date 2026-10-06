#pragma once
#include "ui_shared.h"

struct scrollInfo_s
{
    int nextScrollTime;
    int nextAdjustTime;
    int adjustValue;
    int scrollKey;
    float xStart;
    float yStart;
    itemDef_s* item;
    int scrollDir;
};

void __cdecl Scroll_ListBox_AutoFunc(UiContext* dc, void* data);
void __cdecl Scroll_ListBox_ThumbFunc(UiContext* dc, void* data);
void __cdecl Scroll_Slider_ThumbFunc(UiContext* dc, void* data);
void __cdecl Scroll_Slider_SetThumbPos(UiContext* dc, itemDef_s* item);
