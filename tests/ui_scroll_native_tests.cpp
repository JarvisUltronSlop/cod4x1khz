#include <ui/ui_scroll_capture.h>
#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(v) do { if (!(v)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #v); std::abort(); } } while (0)
static itemDef_s* expected;
static UiContext* context;
static int sliderCalls, keyCalls;
static int maxScroll = 100;
void MyAssertHandler(const char*, int, int, const char*, ...) { std::abort(); }
void Scroll_Slider_SetThumbPos(UiContext* dc, itemDef_s* item) {
    CHECK(dc == context && item == expected); ++sliderCalls;
}
listBoxDef_s* Item_GetListBoxDef(itemDef_s* item) { CHECK(item == expected); return item->typeData.listBox; }
int Item_ListBox_MaxScroll(int client, itemDef_s* item) { CHECK(client == 0 && item == expected); return maxScroll; }
int Item_ListBox_HandleKey(UiContext* dc, itemDef_s* item, int key, int down, int force) {
    CHECK(dc == context && item == expected && key == K_MOUSE1 && down == 1 && force == 0); ++keyCalls; return 1;
}
int main() {
    SYSTEM_INFO sys{}; GetSystemInfo(&sys);
    auto* pages = static_cast<unsigned char*>(VirtualAlloc(nullptr, sys.dwPageSize * 2,
        MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
    CHECK(pages);
    DWORD previous;
    CHECK(VirtualProtect(pages + sys.dwPageSize, sys.dwPageSize, PAGE_NOACCESS, &previous));
    // The old pointer-array index 6 reads eight bytes beyond this native x64
    // capture. A guard page makes the original volume-drag defect deterministic.
    auto* scroll = reinterpret_cast<scrollInfo_s*>(pages + sys.dwPageSize - sizeof(scrollInfo_s));
    *scroll = {};
    auto* item = static_cast<itemDef_s*>(VirtualAlloc(nullptr, sizeof(itemDef_s),
        MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE));
    CHECK(item);
    if constexpr (sizeof(void*) == 8) CHECK(reinterpret_cast<uintptr_t>(item) > UINT32_MAX);
    expected = item;
    UiContext dc{}; context = &dc;
    scroll->item = item; scroll->scrollKey = K_MOUSE1;
    Scroll_Slider_ThumbFunc(&dc, scroll);
    CHECK(sliderCalls == 1);
    listBoxDef_s list{}; item->typeData.listBox = &list;
    item->window.rect = {10, 20, 150, 150, 0, 0};
    dc.realTime = 1000; scroll->nextScrollTime = 100; scroll->nextAdjustTime = 100; scroll->adjustValue = 500;
    Scroll_ListBox_AutoFunc(&dc, scroll);
    CHECK(keyCalls == 1 && scroll->nextScrollTime == 1500 && scroll->nextAdjustTime == 1150 && scroll->adjustValue == 460);
    Scroll_ListBox_AutoFunc(&dc, scroll); CHECK(keyCalls == 1);
    dc.isCursorVisible = true;
    dc.cursor = {85, 95};
    Scroll_ListBox_ThumbFunc(&dc, scroll);
    CHECK(list.startPos[0] == 50 && scroll->yStart == 95);
    dc.cursor.y = -100; Scroll_ListBox_ThumbFunc(&dc, scroll); CHECK(list.startPos[0] == 0);
    dc.cursor.y = 1000; Scroll_ListBox_ThumbFunc(&dc, scroll); CHECK(list.startPos[0] == 100);
    item->window.staticFlags = 0x200000;
    Scroll_ListBox_ThumbFunc(&dc, scroll); CHECK(list.startPos[0] == 50 && scroll->xStart == 85);
    dc.realTime = 2000; Scroll_ListBox_ThumbFunc(&dc, scroll); CHECK(keyCalls == 1); // Unchanged horizontal cursor.
    dc.cursor.x = -100; Scroll_ListBox_ThumbFunc(&dc, scroll); CHECK(list.startPos[0] == 0 && keyCalls == 2);
    dc.cursor.x = 1000; Scroll_ListBox_ThumbFunc(&dc, scroll); CHECK(list.startPos[0] == 100);
    dc.isCursorVisible = false; dc.cursor.x = 85;
    Scroll_ListBox_ThumbFunc(&dc, scroll); CHECK(list.startPos[0] == 100);
    dc.isCursorVisible = true; item->window.rect.w = 40;
    Scroll_ListBox_ThumbFunc(&dc, scroll); CHECK(list.startPos[0] == 0);
    CHECK(VirtualFree(item, 0, MEM_RELEASE) && VirtualFree(pages, 0, MEM_RELEASE));
    std::puts("Native slider/list capture pointer, geometry and repeat timing passed");
}
