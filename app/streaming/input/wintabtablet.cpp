#include "wintabtablet.h"
#include "wintababi.h"
#include "wintabmapping.h"

#include <Limelight.h>
#include <SDL_syswm.h>
#include <CommCtrl.h>
#include <array>
#include <string>
#include <unordered_map>

struct WinTabTablet::Impl {
    SDL_Window* window;
    HWND hwnd = nullptr;
    HMODULE library = nullptr;
    HANDLE context = nullptr;
    WinTabAbi::Info info = nullptr;
    WinTabAbi::Open open = nullptr;
    WinTabAbi::Close close = nullptr;
    WinTabAbi::GetPacket packet = nullptr;
    WinTabAbi::Enable enable = nullptr;
    WinTabAbi::Overlap overlap = nullptr;
    WinTabAbi::Context settings {};
    WinTabAbi::Axis pressureAxis {};
    std::array<WinTabAbi::Axis, 3> orientationAxes {};
    int streamWidth, streamHeight;
    bool wantsCapture = false;
    bool focused = false;
    bool capturing = false;
    bool penProximity = false;
    bool inVideo = false;
    bool down = false;
    uint8_t tool = LI_TOOL_TYPE_PEN;
    float x = 0, y = 0;
    std::unordered_map<HANDLE, bool> tabletMouseDevices;

    struct Cursor {
        bool pen;
        uint8_t tip;
        std::array<uint8_t, 3> barrel;
    };
    std::unordered_map<UINT, Cursor> cursors;

    Impl(SDL_Window* window, int streamWidth, int streamHeight)
        : window(window), streamWidth(streamWidth), streamHeight(streamHeight)
    {
        if (!(LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "WinTab tablet capture requested but host has no native pen support");
            return;
        }
        SDL_SysWMinfo wmInfo;
        SDL_VERSION(&wmInfo.version);
        if (!SDL_GetWindowWMInfo(window, &wmInfo) || wmInfo.subsystem != SDL_SYSWM_WINDOWS) {
            return;
        }
        hwnd = wmInfo.info.win.window;
        wchar_t directory[MAX_PATH];
        const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            return;
        }
        // Never search the application/current directory for an optional driver DLL.
        const std::wstring dll = std::wstring(directory, length) + L"\\Wintab32.dll";
        library = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "WinTab tablet capture unavailable: installed WinTab driver DLL not found");
            return;
        }
        info = reinterpret_cast<WinTabAbi::Info>(GetProcAddress(library, "WTInfoA"));
        open = reinterpret_cast<WinTabAbi::Open>(GetProcAddress(library, "WTOpenA"));
        close = reinterpret_cast<WinTabAbi::Close>(GetProcAddress(library, "WTClose"));
        packet = reinterpret_cast<WinTabAbi::GetPacket>(GetProcAddress(library, "WTPacket"));
        enable = reinterpret_cast<WinTabAbi::Enable>(GetProcAddress(library, "WTEnable"));
        overlap = reinterpret_cast<WinTabAbi::Overlap>(GetProcAddress(library, "WTOverlap"));
        if (!info || !open || !close || !packet || !enable || !overlap ||
                info(WinTabAbi::defaultSystemContext, 0, &settings) != sizeof(settings)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "WinTab tablet capture unavailable: driver API initialization failed");
            return;
        }
        const UINT device = WinTabAbi::deviceCategory + settings.device;
        if (info(device, WinTabAbi::deviceNormalPressure, &pressureAxis) != sizeof(pressureAxis) ||
                pressureAxis.maximum <= pressureAxis.minimum) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "WinTab tablet capture unavailable: tablet has no usable pressure axis");
            return;
        }
        const bool tilt = info(device, WinTabAbi::deviceOrientation, orientationAxes.data()) == sizeof(orientationAxes) &&
                orientationAxes[0].resolution != 0 && orientationAxes[1].resolution != 0;
        if (!tilt) {
            orientationAxes = {};
        }
        strcpy_s(settings.name, "Moonlight WinTab pen input");
        // Use the driver's system mapping, but leave global cursor emulation
        // disabled for this private context. Physical mouse input remains enabled.
        settings.options = (settings.options | WinTabAbi::messages) & ~WinTabAbi::systemCursor;
        settings.packetData = WinTabAbi::packetStatus | WinTabAbi::packetCursor | WinTabAbi::packetButtons |
                WinTabAbi::packetX | WinTabAbi::packetY | WinTabAbi::packetPressure |
                (tilt ? WinTabAbi::packetOrientation : 0);
        settings.packetMode = 0; // Absolute button mask, axes and pressure.
        settings.moveMask = settings.packetData;
        settings.buttonUpMask = settings.buttonDownMask;
        if (!SetWindowSubclass(hwnd, windowProcedure, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this))) {
            return;
        }
        context = open(hwnd, &settings, FALSE);
        if (!context) {
            RemoveWindowSubclass(hwnd, windowProcedure, reinterpret_cast<UINT_PTR>(this));
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "WinTab tablet capture unavailable: opening the tablet context failed");
            return;
        }
        focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "WinTab pen capture ready (pressure range %ld..%ld, tilt %s)",
                    pressureAxis.minimum, pressureAxis.maximum, tilt ? "supported" : "unavailable");
    }

    ~Impl()
    {
        if (context) {
            cancel();
            enable(context, FALSE);
            close(context);
            RemoveWindowSubclass(hwnd, windowProcedure, reinterpret_cast<UINT_PTR>(this));
        }
        if (library) {
            FreeLibrary(library);
        }
    }

    void send(uint8_t event, uint8_t buttons = 0, float pressure = 0,
              uint16_t rotation = LI_ROT_UNKNOWN, uint8_t tilt = LI_TILT_UNKNOWN)
    {
        LiSendPenEvent(event, tool, buttons, x, y, pressure, 0, 0, rotation, tilt);
    }

    void cancel()
    {
        if (inVideo || down) {
            send(LI_TOUCH_EVENT_CANCEL_ALL);
            send(LI_TOUCH_EVENT_HOVER_LEAVE);
        }
        inVideo = down = penProximity = false;
    }

    void updateCapture()
    {
        const bool next = wantsCapture && focused && context;
        if (capturing == next) {
            return;
        }
        if (!next) {
            cancel();
        }
        capturing = next;
        if (context) {
            enable(context, next);
            if (next) {
                overlap(context, TRUE);
            }
        }
    }

    const Cursor& cursor(UINT id)
    {
        auto existing = cursors.find(id);
        if (existing != cursors.end()) {
            return existing->second;
        }
        Cursor result {false, 0xff, {0xff, 0xff, 0xff}};
        DWORD data = 0;
        BYTE tip = 0xff, count = 0;
        std::array<BYTE, 32> map {};
        const UINT category = WinTabAbi::cursorCategory + id;
        info(category, WinTabAbi::cursorPacketData, &data);
        result.pen = (data & WinTabAbi::packetPressure) != 0;
        info(category, WinTabAbi::cursorNormalPressureButton, &tip);
        info(category, WinTabAbi::cursorButtonCount, &count);
        if (info(category, WinTabAbi::cursorButtonMap, map.data()) == map.size()) {
            if (tip < map.size()) {
                result.tip = map[tip];
            }
            unsigned int barrel = 0;
            for (unsigned int button = 0; button < count && button < map.size() && barrel < result.barrel.size(); button++) {
                if (button != tip && map[button] < 32) {
                    result.barrel[barrel++] = map[button];
                }
            }
        }
        return cursors.emplace(id, result).first->second;
    }

    void handlePacket(UINT serial)
    {
        WinTabAbi::Packet data {};
        if (!capturing || !packet(context, serial, &data)) {
            return;
        }
        const auto& cursorInfo = cursor(data.cursor);
        if (!cursorInfo.pen || (data.status & WinTabAbi::proximityStatus)) {
            cancel();
            return;
        }
        if (data.status & WinTabAbi::queueErrorStatus) {
            cancel(); // Do not leave a tip/barrel button stuck after losing packets.
        }
        penProximity = true;
        POINT position {data.x, data.y};
        RECT rect;
        if (!ScreenToClient(hwnd, &position) || !GetClientRect(hwnd, &rect)) {
            cancel();
            return;
        }
        const auto mapped = WinTabMapping::point(position.x, position.y, rect.right, rect.bottom, streamWidth, streamHeight);
        if (!mapped.inside && !down) {
            if (inVideo) {
                send(LI_TOUCH_EVENT_HOVER_LEAVE);
                inVideo = false;
            }
            return;
        }
        const uint8_t nextTool = (data.status & WinTabAbi::invertedStatus) ? LI_TOOL_TYPE_ERASER : LI_TOOL_TYPE_PEN;
        if (nextTool != tool && inVideo) {
            cancel();
            penProximity = true;
        }
        tool = nextTool;
        x = mapped.x;
        y = mapped.y;
        uint8_t buttons = 0;
        for (unsigned int index = 0; index < cursorInfo.barrel.size(); index++) {
            if (cursorInfo.barrel[index] < 32 && (data.buttons & (1u << cursorInfo.barrel[index]))) {
                buttons |= 1u << index;
            }
        }
        const float normalizedPressure = WinTabMapping::pressure(data.pressure, pressureAxis.minimum, pressureAxis.maximum);
        const bool nextDown = cursorInfo.tip < 32 ? (data.buttons & (1u << cursorInfo.tip)) != 0 : normalizedPressure > 0;
        const uint8_t event = nextDown ? (down ? LI_TOUCH_EVENT_MOVE : LI_TOUCH_EVENT_DOWN) : (down ? LI_TOUCH_EVENT_UP : LI_TOUCH_EVENT_HOVER);
        const auto orientation = WinTabMapping::orientation(data.orientation.azimuth, data.orientation.altitude,
                                                           orientationAxes[0].resolution, orientationAxes[1].resolution);
        send(event, buttons, nextDown ? normalizedPressure : 0,
             orientation.rotation, orientation.tilt);
        down = nextDown;
        inVideo = true;
    }

    bool isTabletRawMouse(LPARAM parameter)
    {
        RAWINPUT raw {};
        UINT size = sizeof(raw);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(parameter), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) == UINT(-1) ||
                raw.header.dwType != RIM_TYPEMOUSE) {
            return false;
        }
        auto found = tabletMouseDevices.find(raw.header.hDevice);
        if (found != tabletMouseDevices.end()) {
            return found->second;
        }
        std::array<wchar_t, 512> name {};
        size = static_cast<UINT>(name.size());
        const bool tablet = GetRawInputDeviceInfoW(raw.header.hDevice, RIDI_DEVICENAME, name.data(), &size) != UINT(-1) &&
                (wcsstr(name.data(), L"VID_056A") || wcsstr(name.data(), L"vid_056a"));
        tabletMouseDevices.emplace(raw.header.hDevice, tablet);
        return tablet;
    }

    static LRESULT CALLBACK windowProcedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR reference)
    {
        auto& self = *reinterpret_cast<Impl*>(reference);
        if (message == self.settings.messageBase && reinterpret_cast<HANDLE>(lParam) == self.context) {
            self.handlePacket(static_cast<UINT>(wParam));
            return 0;
        }
        if (message == self.settings.messageBase + 5 && reinterpret_cast<HANDLE>(wParam) == self.context) {
            if (LOWORD(lParam) == 0) {
                self.cancel();
            }
            return 0;
        }
        if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
            self.focused = message == WM_SETFOCUS;
            self.updateCapture();
        }
        if (message == self.settings.messageBase + 2 || message == self.settings.messageBase + 6 || message == WM_DEVICECHANGE) {
            self.cancel();
            self.cursors.clear();
            self.tabletMouseDevices.clear();
        }
        if (self.capturing && self.penProximity) {
            // Suppress only tablet-generated duplicates, never every mouse event
            // merely because the pen is nearby. A separate mouse remains usable.
            if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) {
                const DWORD extra = static_cast<DWORD>(GetMessageExtraInfo());
                if ((extra & 0xffffff00) == 0xff515700 && !(extra & 0x80)) {
                    return 0;
                }
            }
            if (message == WM_INPUT && self.isTabletRawMouse(lParam)) {
                return DefWindowProcW(hwnd, message, wParam, lParam);
            }
            if (message == WM_POINTERDOWN || message == WM_POINTERUPDATE || message == WM_POINTERUP) {
                POINTER_INPUT_TYPE type;
                if (GetPointerType(GET_POINTERID_WPARAM(wParam), &type) && type == PT_PEN) {
                    return 0;
                }
            }
        }
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }
};

WinTabTablet::WinTabTablet(SDL_Window* window, int streamWidth, int streamHeight)
    : m_Impl(std::make_unique<Impl>(window, streamWidth, streamHeight)) {}
WinTabTablet::~WinTabTablet() = default;
bool WinTabTablet::available() const { return m_Impl->context != nullptr; }
bool WinTabTablet::isCapturing() const { return m_Impl->capturing; }
void WinTabTablet::setActive(bool active) { m_Impl->wantsCapture = active; m_Impl->updateCapture(); }
void WinTabTablet::cancel() { m_Impl->cancel(); }
