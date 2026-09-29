#include "display/ExistingMonitor.h"

#include <cwchar>

namespace od {

namespace {

bool DeviceLooksLikeMtt(const DISPLAY_DEVICEW& dev)
{
    // Hardware ID looks like DISPLAY\MTT1337\... ; string is "Generic Monitor (VDD by MTT)"
    if (wcsstr(dev.DeviceID, L"MTT1337") != nullptr)
        return true;
    if (wcsstr(dev.DeviceString, L"VDD by MTT") != nullptr)
        return true;
    return false;
}

} // namespace

bool GetMonitorRectByDeviceName(const std::wstring& deviceName, RECT& out)
{
    struct Ctx {
        const std::wstring* name;
        RECT rect{};
        bool found = false;
    } ctx{&deviceName};

    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(mon, &mi))
                return TRUE;
            if (c->name->compare(mi.szDevice) == 0) {
                c->rect = mi.rcMonitor;
                c->found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    if (!ctx.found)
        return false;
    out = ctx.rect;
    return true;
}

bool FindMttVirtualMonitor(ExistingMonitor& out)
{
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW adapter{};
        adapter.cb = sizeof(adapter);
        if (!EnumDisplayDevicesW(nullptr, i, &adapter, 0))
            break;
        if ((adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0)
            continue;

        for (DWORD m = 0;; ++m) {
            DISPLAY_DEVICEW mon{};
            mon.cb = sizeof(mon);
            if (!EnumDisplayDevicesW(adapter.DeviceName, m, &mon, 0))
                break;
            if ((mon.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0)
                continue;
            if (!DeviceLooksLikeMtt(mon) && !DeviceLooksLikeMtt(adapter))
                continue;

            // DXGI / Desktop Duplication want the adapter GDI name (\\.\DISPLAYn),
            // not EnumDisplayDevices' \\.\DISPLAYn\Monitor0 form.
            ExistingMonitor found;
            found.deviceName = adapter.DeviceName;
            found.deviceString = mon.DeviceString[0] ? mon.DeviceString : adapter.DeviceString;
            found.deviceId = mon.DeviceID[0] ? mon.DeviceID : adapter.DeviceID;
            if (!GetMonitorRectByDeviceName(found.deviceName, found.rect))
                continue;
            out = std::move(found);
            return true;
        }
    }
    return false;
}


bool EnsureMonitorRefresh(const std::wstring& deviceName, uint32_t fps)
{
    if (fps == 0)
        return false;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm))
        return false;
    if (dm.dmDisplayFrequency == static_cast<DWORD>(fps))
        return true;
    dm.dmDisplayFrequency = static_cast<DWORD>(fps);
    dm.dmFields = DM_DISPLAYFREQUENCY;
    const LONG r = ChangeDisplaySettingsExW(deviceName.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
    return r == DISP_CHANGE_SUCCESSFUL;
}

bool EnsureMonitorMode(const std::wstring& deviceName, uint32_t width, uint32_t height, uint32_t fps)
{
    if (width == 0 || height == 0)
        return false;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm))
        return false;

    const bool orientOk = dm.dmDisplayOrientation == DMDO_DEFAULT;
    const bool sizeOk = dm.dmPelsWidth == width && dm.dmPelsHeight == height;
    const bool fpsOk = fps == 0 || dm.dmDisplayFrequency == static_cast<DWORD>(fps);
    if (orientOk && sizeOk && fpsOk)
        return true;

    dm.dmPelsWidth = width;
    dm.dmPelsHeight = height;
    dm.dmDisplayOrientation = DMDO_DEFAULT;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYORIENTATION;
    if (fps != 0) {
        dm.dmDisplayFrequency = static_cast<DWORD>(fps);
        dm.dmFields |= DM_DISPLAYFREQUENCY;
    }
    const LONG r = ChangeDisplaySettingsExW(deviceName.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
    return r == DISP_CHANGE_SUCCESSFUL;
}

} // namespace od
