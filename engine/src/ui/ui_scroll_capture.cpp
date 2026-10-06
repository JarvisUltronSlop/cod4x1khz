#include "ui_scroll_capture.h"
#include <algorithm>

void __cdecl Scroll_ListBox_AutoFunc(UiContext* dc, void* data)
{
    auto& scroll = *static_cast<scrollInfo_s*>(data);
    if (dc->realTime > scroll.nextScrollTime)
    {
        Item_ListBox_HandleKey(dc, scroll.item, scroll.scrollKey, 1, 0);
        scroll.nextScrollTime = scroll.adjustValue + dc->realTime;
    }
    if (dc->realTime > scroll.nextAdjustTime)
    {
        scroll.nextAdjustTime = dc->realTime + 150;
        if (scroll.adjustValue > 20) scroll.adjustValue -= 40;
    }
}

void __cdecl Scroll_ListBox_ThumbFunc(UiContext* dc, void* data)
{
    auto& scroll = *static_cast<scrollInfo_s*>(data);
    if (!dc->isCursorVisible) return;
    auto* list = Item_GetListBoxDef(scroll.item);
    if (!list) return;
    const auto& rect = scroll.item->window.rect;
    const bool horizontal = (scroll.item->window.staticFlags & 0x200000) != 0;
    const float cursor = horizontal ? dc->cursor.x : dc->cursor.y;
    auto& previous = horizontal ? scroll.xStart : scroll.yStart;
    if (horizontal && previous == cursor) return;
    if (previous != cursor)
    {
        const int maximum = std::max(0, Item_ListBox_MaxScroll(dc->localClientNum, scroll.item));
        const float travel = (horizontal ? rect.w : rect.h) - 50.0f;
        const float start = (horizontal ? rect.x : rect.y) + 25.0f;
        // No thumb travel is available in a collapsed/short list rectangle.
        const int position = travel > 0.0f ? static_cast<int>((cursor - start) * static_cast<double>(maximum) / travel) : 0;
        list->startPos[dc->localClientNum] = std::clamp(position, 0, maximum);
        previous = cursor;
    }
    Scroll_ListBox_AutoFunc(dc, data);
}

void __cdecl Scroll_Slider_ThumbFunc(UiContext* dc, void* data)
{
    // Capture data is a struct, not an array of item pointers. Pointer index 6
    // assumed a four-byte pointer and reads beyond the native x64 capture.
    auto& scroll = *static_cast<scrollInfo_s*>(data);
    Scroll_Slider_SetThumbPos(dc, scroll.item);
}
