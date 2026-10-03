#include "khz_input_owner.h"
#include "khz_input_model.h"
#include <Windows.h>
#include <array>
#include <atomic>
#include <limits>

namespace {
constexpr unsigned Capacity = 4096;
constexpr UINT ControlMessage = WM_APP + 0x4b1;
constexpr UINT WatchdogTimer = 0x4b1;
constexpr wchar_t WindowClass[] = L"cod4x1khz.RawInputOwner";

using InputRing = khz_input::Ring<KHZ_OwnedInputEvent, Capacity>;
khz_input::OwnershipTimeline ownershipHistory;
khz_input::ControlState producerControl;

InputRing queue;
HANDLE producerThread = nullptr, readyEvent = nullptr, wakeEvent = nullptr;
std::atomic<std::uintptr_t> messageWindow{0}, mainWindow{0};
std::atomic<bool> stopping{false}, running{false}, rawDevicesOwned{false};
std::atomic<bool> requestedGameplay{false}, parked{true}, activeGameplay{false}, focused{false};
std::atomic<std::uint64_t> requestedEpoch{0};
std::atomic<std::uint64_t> controlVersion{0},producerControlVersion{0};
std::atomic<std::uint64_t> produced{0}, consumed{0}, dropped{0}, resets{0}, focusChanges{0};
std::atomic<std::uint64_t> windowRebinds{0},windowRebindFailures{0};
std::atomic<unsigned> lastError{0}, producerThreadId{0};
std::atomic<bool> overflowReset{false};
std::atomic<std::uint64_t> overflowSequence{0}, overflowQpc{0};

// All fields below are touched exclusively by the input producer.
std::uint64_t producerSequence = 0, producerEpoch = 0;
std::uint64_t producerMinimumMessageTime = 0;
bool producerActive = false, producerFocus = false, producerParked = true;
std::array<bool, 256> blockedKeys{};
unsigned blockedButtons = 0;
struct AbsoluteDevice { HANDLE device = nullptr; LONG x = 0, y = 0; bool used = false; };
std::array<AbsoluteDevice, 16> absoluteDevices{};

std::uint64_t Now()
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
}
bool ForegroundGame()
{
    const auto window = reinterpret_cast<HWND>(mainWindow.load(std::memory_order_acquire));
    return window && IsWindow(window) && GetForegroundWindow() == window && !IsIconic(window);
}
void BlockHeld()
{
    for (unsigned key = 0; key < blockedKeys.size(); ++key) blockedKeys[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    constexpr int buttons[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
    blockedButtons = 0;
    for (unsigned i = 0; i < 5; ++i) if (GetAsyncKeyState(buttons[i]) & 0x8000) blockedButtons |= 1u << i;
    absoluteDevices = {};
}
bool Push(KHZ_OwnedInputEvent event)
{
    event.qpc = Now();
    event.sequence = ++producerSequence;
    event.controlEpoch = producerEpoch;
    event.messageTimeMs = static_cast<unsigned>(GetMessageTime());
    if (!queue.Push(event)) {
        dropped.fetch_add(1, std::memory_order_relaxed);
        overflowSequence.store(event.sequence, std::memory_order_relaxed);
        overflowQpc.store(event.qpc, std::memory_order_relaxed);
        overflowReset.store(true, std::memory_order_release);
        // A missing key/button-up cannot become a permanently held action.
        // The consumer releases everything; the producer requires fresh presses.
        BlockHeld();
        return false;
    }
    produced.fetch_add(1, std::memory_order_relaxed);
    return true;
}
void Reset(KHZ_InputResetReason reason)
{
    // Raw Input may already be queued on the producer window when a new focus
    // or control epoch begins. Do not relabel those old packets as fresh input.
    producerMinimumMessageTime = GetTickCount64();
    KHZ_OwnedInputEvent event;
    event.kind = KHZ_OwnedInputKind::Reset;
    event.resetReason = reason;
    Push(event);
    resets.fetch_add(1, std::memory_order_relaxed);
    BlockHeld();
}
void RefreshControl()
{
    const auto version=controlVersion.load(std::memory_order_acquire);
    if(version&1)return;
    const auto epoch = requestedEpoch.load(std::memory_order_acquire);
    const bool nowParked = parked.load(std::memory_order_acquire);
    const bool nowFocused = ForegroundGame();
    const bool requested=requestedGameplay.load(std::memory_order_acquire);
    if(controlVersion.load(std::memory_order_acquire)!=version)return;
    KHZ_InputResetReason reason{};
    const bool previouslyParked=producerControl.parked;
    const bool focusChanged = nowFocused != producerControl.focused;
    const bool changed = producerControl.Refresh(requested,
        nowParked, nowFocused, epoch, &reason);
    producerEpoch = producerControl.epoch;
    producerFocus = producerControl.focused;
    producerParked = producerControl.parked;
    producerActive = producerControl.active;
    focused.store(nowFocused, std::memory_order_release);
    if (focusChanged) focusChanges.fetch_add(1, std::memory_order_relaxed);
    if (changed) {
        // A producer that already acknowledged Park does not enqueue fresh
        // focus resets behind a checkpoint's drained fence. Resume still emits
        // its real reset and requires fresh physical presses.
        if(previouslyParked&&producerParked)BlockHeld();
        else Reset(reason);
    }
    activeGameplay.store(producerActive, std::memory_order_release);
    producerControlVersion.store(version,std::memory_order_release);
}

void ReadInput(HRAWINPUT handle)
{
    RefreshControl();
    RAWINPUT input{};
    UINT size = sizeof(input);
    const UINT bytes = GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER));
    if (bytes == static_cast<UINT>(-1) || bytes > sizeof(input) || bytes < sizeof(RAWINPUTHEADER)) {
        lastError.store(GetLastError(), std::memory_order_relaxed);
        return;
    }
    if (!producerActive) return;
    const std::uint64_t messageNow = GetTickCount64();
    const unsigned messageAge = unsigned(messageNow) - static_cast<unsigned>(GetMessageTime());
    if (messageAge > 0x7fffffffu || messageNow < messageAge
        || messageNow - messageAge < producerMinimumMessageTime) return;
    if (input.header.dwType == RIM_TYPEKEYBOARD) {
        const auto& key = input.data.keyboard;
        if (key.VKey == 255 || key.VKey >= blockedKeys.size() || key.MakeCode == KEYBOARD_OVERRUN_MAKE_CODE) return;
        const bool down = !(key.Flags & RI_KEY_BREAK);
        if (blockedKeys[key.VKey]) {
            if (!down) blockedKeys[key.VKey] = false;
            return;
        }
        KHZ_OwnedInputEvent event;
        event.kind = KHZ_OwnedInputKind::Key;
        event.virtualKey = key.VKey;
        event.scanCode = key.MakeCode;
        event.keyboardFlags = key.Flags;
        event.keyDown = down;
        Push(event);
    } else if (input.header.dwType == RIM_TYPEMOUSE) {
        const auto& mouse = input.data.mouse;
        KHZ_OwnedInputEvent event;
        event.kind = KHZ_OwnedInputKind::Mouse;
        event.dx = mouse.lLastX;
        event.dy = mouse.lLastY;
        if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
            AbsoluteDevice* absolute = nullptr;
            for (auto& device : absoluteDevices)
                if (device.used && device.device == input.header.hDevice) { absolute = &device; break; }
            if (!absolute) for (auto& device : absoluteDevices)
                if (!device.used) { absolute = &device; device.device = input.header.hDevice; break; }
            if (!absolute) { dropped.fetch_add(1, std::memory_order_relaxed); return; }
            const LONG width = GetSystemMetrics(mouse.usFlags & MOUSE_VIRTUAL_DESKTOP ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
            const LONG height = GetSystemMetrics(mouse.usFlags & MOUSE_VIRTUAL_DESKTOP ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
            const LONG x = static_cast<LONG>((static_cast<long long>(mouse.lLastX) * width) / 65535);
            const LONG y = static_cast<LONG>((static_cast<long long>(mouse.lLastY) * height) / 65535);
            event.dx = absolute->used ? x - absolute->x : 0;
            event.dy = absolute->used ? y - absolute->y : 0;
            absolute->x = x; absolute->y = y; absolute->used = true;
        }
        event.mouseButtonFlags = mouse.usButtonFlags;
        for (unsigned i = 0; i < 5; ++i) {
            const unsigned down = 1u << (i * 2), up = down << 1;
            if (blockedButtons & (1u << i)) {
                if (event.mouseButtonFlags & up) blockedButtons &= ~(1u << i);
                event.mouseButtonFlags &= ~(down | up);
            }
        }
        if (mouse.usButtonFlags & (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL)) event.wheelDelta = static_cast<SHORT>(mouse.usButtonData);
        if (event.dx || event.dy || event.mouseButtonFlags || event.wheelDelta) Push(event);
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_INPUT) {
        ReadInput(reinterpret_cast<HRAWINPUT>(lparam));
        return DefWindowProcW(window, message, wparam, lparam);
    }
    if (message == ControlMessage || message == WM_TIMER) { RefreshControl(); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
void CALLBACK ForegroundChanged(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) { RefreshControl(); }

DWORD WINAPI Producer(void*)
{
    producerThreadId.store(GetCurrentThreadId(), std::memory_order_release);
    const auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = WindowClass;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        lastError.store(GetLastError(), std::memory_order_relaxed);
        SetEvent(readyEvent);
        return 1;
    }
    const HWND window = CreateWindowExW(0, WindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (!window) { lastError.store(GetLastError(), std::memory_order_relaxed); SetEvent(readyEvent); return 1; }
    const RAWINPUTDEVICE devices[] = { {1, 2, RIDEV_INPUTSINK, window}, {1, 6, RIDEV_INPUTSINK, window} };
    if (!RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE))) {
        lastError.store(GetLastError(), std::memory_order_relaxed);
        DestroyWindow(window);
        SetEvent(readyEvent);
        return 1;
    }
    messageWindow.store(reinterpret_cast<std::uintptr_t>(window), std::memory_order_release);
    rawDevicesOwned.store(true, std::memory_order_release);
    const auto foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, ForegroundChanged, 0, 0, WINEVENT_OUTOFCONTEXT);
    // Win32 timers have coarse scheduling on some configurations. Every input
    // packet also checks focus, and the message-loop timeout supplies a 2 ms
    // independent focus/control watchdog even while the frontend is stalled.
    SetTimer(window, WatchdogTimer, 2, nullptr);
    running.store(true, std::memory_order_release);
    RefreshControl();
    SetEvent(readyEvent);
    bool quit = false;
    while (!stopping.load(std::memory_order_acquire) && !quit) {
        const DWORD wait = MsgWaitForMultipleObjectsEx(1, &wakeEvent, 2, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_FAILED) { lastError.store(GetLastError(), std::memory_order_relaxed); break; }
        MSG message;
        // Bound each message batch so a flood cannot starve park/stop requests.
        for (unsigned count = 0; count < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
            if (message.message == WM_QUIT) { quit = true; break; }
            DispatchMessageW(&message);
        }
        RefreshControl();
    }
    producerActive = false;
    activeGameplay.store(false, std::memory_order_release);
    Reset(KHZ_InputResetReason::Stop);
    if (foregroundHook) UnhookWinEvent(foregroundHook);
    KillTimer(window, WatchdogTimer);
    const RAWINPUTDEVICE remove[] = { {1, 2, RIDEV_REMOVE, nullptr}, {1, 6, RIDEV_REMOVE, nullptr} };
    if (!RegisterRawInputDevices(remove, 2, sizeof(RAWINPUTDEVICE))) lastError.store(GetLastError(), std::memory_order_relaxed);
    rawDevicesOwned.store(false, std::memory_order_release);
    messageWindow.store(0, std::memory_order_release);
    DestroyWindow(window);
    running.store(false, std::memory_order_release);
    return 0;
}
void Wake()
{
    if (wakeEvent) SetEvent(wakeEvent);
    if (const auto window = reinterpret_cast<HWND>(messageWindow.load(std::memory_order_acquire)))
        PostMessageW(window, ControlMessage, 0, 0);
}
}

bool KHZ_InputOwnerStart(std::uintptr_t window)
{
    if (!window || !IsWindow(reinterpret_cast<HWND>(window))) return false;
    bool rebound=false;
    if(producerThread){
        const auto disposition=[&](){return khz_input::WindowStartPolicy(mainWindow.load(std::memory_order_acquire),window,
            running.load(std::memory_order_acquire)&&!stopping.load(std::memory_order_acquire),parked.load(std::memory_order_acquire),KHZ_InputOwnerParkAcknowledged());};
        auto action=disposition();
        if(action==khz_input::WindowStart::AwaitPark){
            // Renderer/map lifecycle work stopped the sole simulation consumer.
            // Wait for the independent producer to acknowledge that requested
            // park before unregistering its devices or reclaiming its queue.
            const auto until=GetTickCount64()+2000;Wake();
            do{Sleep(1);action=disposition();}while(action==khz_input::WindowStart::AwaitPark&&GetTickCount64()<until);
        }
        if(action==khz_input::WindowStart::Reuse)return true;
        if(action!=khz_input::WindowStart::Recycle){
            lastError.store(action==khz_input::WindowStart::AwaitPark?ERROR_TIMEOUT:ERROR_BUSY,std::memory_order_relaxed);
            windowRebindFailures.fetch_add(1,std::memory_order_relaxed);return false;
        }
        if(!KHZ_InputOwnerStop()){windowRebindFailures.fetch_add(1,std::memory_order_relaxed);return false;}
        rebound=true;
    }
    // The control owner has quiesced the sole consumer before starting a new run.
    queue.ResetQuiescent();
    produced = 0; consumed = 0; dropped = 0; resets = 0; focusChanges = 0;
    lastError = 0; overflowReset = false; overflowSequence = 0; overflowQpc = 0;
    producerSequence = 0; producerEpoch = 0; producerActive = false; producerFocus = false; producerParked = true;
    producerControl = {};
    mainWindow.store(window, std::memory_order_release);
    requestedGameplay = false; requestedEpoch = 0; parked = true; focused = false; stopping = false;
    controlVersion=0;producerControlVersion=0;
    readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!readyEvent || !wakeEvent) { lastError = GetLastError(); KHZ_InputOwnerStop(); if(rebound)windowRebindFailures.fetch_add(1,std::memory_order_relaxed); return false; }
    producerThread = CreateThread(nullptr, 0, Producer, nullptr, 0, nullptr);
    if (!producerThread) { lastError = GetLastError(); KHZ_InputOwnerStop(); if(rebound)windowRebindFailures.fetch_add(1,std::memory_order_relaxed); return false; }
    if (WaitForSingleObject(readyEvent, 2000) != WAIT_OBJECT_0 || !running.load(std::memory_order_acquire)) {
        KHZ_InputOwnerStop();
        if(rebound)windowRebindFailures.fetch_add(1,std::memory_order_relaxed);
        return false;
    }
    if(rebound)windowRebinds.fetch_add(1,std::memory_order_relaxed);
    return true;
}
void KHZ_InputOwnerSetGameplay(bool enabled, std::uint64_t epoch)
{
    // Main is the sole writer. Per-frame mask refreshes must not briefly park
    // the independent producer or release held input when nothing changed.
    if (requestedEpoch.load(std::memory_order_acquire) == epoch
        && requestedGameplay.load(std::memory_order_acquire) == enabled) return;
    controlVersion.fetch_add(1,std::memory_order_acq_rel);
    ownershipHistory.Set(enabled && !parked.load(std::memory_order_acquire), epoch, GetTickCount64());
    requestedGameplay.store(false, std::memory_order_release);
    requestedEpoch.store(epoch, std::memory_order_release);
    requestedGameplay.store(enabled, std::memory_order_release);
    controlVersion.fetch_add(1,std::memory_order_release);
    Wake();
}
void KHZ_InputOwnerPark() {
    controlVersion.fetch_add(1,std::memory_order_acq_rel);
    ownershipHistory.Set(false, requestedEpoch.load(std::memory_order_acquire), GetTickCount64());
    parked.store(true, std::memory_order_release);
    controlVersion.fetch_add(1,std::memory_order_release);Wake();
}
void KHZ_InputOwnerResume() {
    controlVersion.fetch_add(1,std::memory_order_acq_rel);
    ownershipHistory.Set(requestedGameplay.load(std::memory_order_acquire), requestedEpoch.load(std::memory_order_acquire), GetTickCount64());
    parked.store(false, std::memory_order_release);
    controlVersion.fetch_add(1,std::memory_order_release);Wake();
}
bool KHZ_InputOwnerStop()
{
    controlVersion.fetch_add(1,std::memory_order_acq_rel);
    ownershipHistory.Set(false, requestedEpoch.load(std::memory_order_acquire), GetTickCount64());
    requestedGameplay.store(false, std::memory_order_release);
    parked.store(true, std::memory_order_release);
    stopping.store(true, std::memory_order_release);
    controlVersion.fetch_add(1,std::memory_order_release);
    Wake();
    if (producerThread) {
        if (WaitForSingleObject(producerThread, 2000) != WAIT_OBJECT_0) {
            lastError.store(ERROR_TIMEOUT, std::memory_order_relaxed);
            return false; // Preserve handles/storage until the producer exits.
        }
        CloseHandle(producerThread); producerThread = nullptr;
    }
    if (readyEvent) { CloseHandle(readyEvent); readyEvent = nullptr; }
    if (wakeEvent) { CloseHandle(wakeEvent); wakeEvent = nullptr; }
    running = false; rawDevicesOwned = false; activeGameplay = false; focused = false;
    return true;
}
bool KHZ_InputOwnerPopDue(KHZ_OwnedInputEvent* event, std::uint64_t deadline)
{
    if (!event) return false;
    if (overflowReset.load(std::memory_order_acquire) && overflowQpc.load(std::memory_order_acquire) <= deadline
        && overflowReset.exchange(false, std::memory_order_acq_rel)) {
        queue.DiscardFromConsumer();
        *event = KHZ_OwnedInputEvent{};
        event->kind = KHZ_OwnedInputKind::Reset;
        event->resetReason = KHZ_InputResetReason::Overflow;
        event->qpc = overflowQpc.load(std::memory_order_acquire);
        event->sequence = overflowSequence.load(std::memory_order_acquire);
        event->controlEpoch = requestedEpoch.load(std::memory_order_acquire);
        consumed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (!queue.Pop(event, deadline)) return false;
    consumed.fetch_add(1, std::memory_order_relaxed);
    return true;
}
bool KHZ_InputOwnerPop(KHZ_OwnedInputEvent* event)
{
    return KHZ_InputOwnerPopDue(event, (std::numeric_limits<std::uint64_t>::max)());
}
void KHZ_InputOwnerDiscardPending() { queue.DiscardFromConsumer();overflowReset.store(false,std::memory_order_release); }
bool KHZ_InputOwnerOwnsGameplay()
{
    return running.load(std::memory_order_acquire) && activeGameplay.load(std::memory_order_acquire)
        && requestedGameplay.load(std::memory_order_acquire) && !parked.load(std::memory_order_acquire);
}
bool KHZ_InputOwnerOwnsRawDevices() { return rawDevicesOwned.load(std::memory_order_acquire); }
bool KHZ_InputOwnerParkAcknowledged() {
    const auto version=controlVersion.load(std::memory_order_acquire);
    return khz_input::ParkAcknowledged(version,producerControlVersion.load(std::memory_order_acquire),
        running.load(std::memory_order_acquire),parked.load(std::memory_order_acquire),activeGameplay.load(std::memory_order_acquire))
        &&controlVersion.load(std::memory_order_acquire)==version;
}
bool KHZ_InputOwnerCheckpointParked() {
    const auto version=controlVersion.load(std::memory_order_acquire);
    return khz_input::CheckpointParked(version,producerControlVersion.load(std::memory_order_acquire),
        running.load(std::memory_order_acquire),parked.load(std::memory_order_acquire),activeGameplay.load(std::memory_order_acquire),
        queue.Size(),overflowReset.load(std::memory_order_acquire))&&controlVersion.load(std::memory_order_acquire)==version;
}
bool KHZ_InputOwnerSuppressLegacy(unsigned messageTimeMs) {
    return ownershipHistory.Suppress(messageTimeMs, GetTickCount64());
}
KHZ_InputOwnerStats KHZ_GetInputOwnerStats()
{
    KHZ_InputOwnerStats stats;
    stats.produced = produced.load(std::memory_order_relaxed); stats.consumed = consumed.load(std::memory_order_relaxed);
    stats.dropped = dropped.load(std::memory_order_relaxed); stats.resets = resets.load(std::memory_order_relaxed);
    stats.focusChanges = focusChanges.load(std::memory_order_relaxed); stats.queued = queue.Size();
    stats.windowRebinds=windowRebinds.load(std::memory_order_relaxed);stats.windowRebindFailures=windowRebindFailures.load(std::memory_order_relaxed);
    stats.lastError = lastError.load(std::memory_order_relaxed); stats.producerThreadId = producerThreadId.load(std::memory_order_relaxed);
    stats.running = running.load(std::memory_order_acquire); stats.gameplay = KHZ_InputOwnerOwnsGameplay();
    stats.focused = focused.load(std::memory_order_acquire); stats.parked = parked.load(std::memory_order_acquire);
    return stats;
}
bool KHZ_InputOwnerSelfTest() { return khz_input::ModelSelfTest(); }
