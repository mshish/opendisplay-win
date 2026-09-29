#pragma once

#include <windows.h>

#include <string>

namespace od {

// Locates an already-attached virtual / physical monitor without creating one.
// Used to point DesktopDuplication at the Intel-pinned MTT Virtual Display
// Driver head instead of creating a Parsec VDD monitor.

struct ExistingMonitor {
    std::wstring deviceName; // e.g. L"\\.\DISPLAY25"
    std::wstring deviceString;
    std::wstring deviceId;
    RECT rect{};
};

// Prefer MTT Virtual Display Driver ("MTT1337" / "VDD by MTT"). Returns false
// if none is currently attached to the desktop.
bool FindMttVirtualMonitor(ExistingMonitor& out);

// Refresh rect for a known GDI device name.
bool GetMonitorRectByDeviceName(const std::wstring& deviceName, RECT& out);

// Best-effort: keep current resolution, set refresh to fps.
bool EnsureMonitorRefresh(const std::wstring& deviceName, uint32_t fps);

// Best-effort: set resolution + DMDO_DEFAULT (landscape) so DXGI capture
// matches hello WxH. Used when Windows is on the swapped (portrait) mode.
bool EnsureMonitorMode(const std::wstring& deviceName, uint32_t width, uint32_t height,
                       uint32_t fps);

} // namespace od
