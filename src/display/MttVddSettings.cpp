#include "display/MttVddSettings.h"

#include "display/ExistingMonitor.h"

#include "app/Log.h"

#include <windows.h>

#include <cfgmgr32.h>
#include <setupapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace od {

namespace {

constexpr wchar_t kLiveSettingsPath[] = L"C:\\VirtualDisplayDriver\\vdd_settings.xml";
constexpr wchar_t kLiveEdidPath[] = L"C:\\VirtualDisplayDriver\\user_edid.bin";
constexpr wchar_t kAppsSettingsPath[] = L"D:\\apps\\VirtualDisplayDriver\\vdd_settings.intel.xml";
constexpr wchar_t kAppsEdidPath[] = L"D:\\apps\\VirtualDisplayDriver\\user_edid.bin";
// Dev-tree fallback when running an unpackaged build from the repo.
constexpr wchar_t kDevEdidPath[] = L"D:\\projects\\opendisplay-win\\assets\\mtt\\user_edid.bin";

// Display adapter class.
const GUID kDisplayClassGuid = {
    0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

struct ModeKey {
    uint32_t w = 0, h = 0, hz = 0;
    bool operator<(const ModeKey& o) const
    {
        if (w != o.w)
            return w < o.w;
        if (h != o.h)
            return h < o.h;
        return hz < o.hz;
    }
    bool operator==(const ModeKey& o) const { return w == o.w && h == o.h && hz == o.hz; }
};

bool FileExists(const wchar_t* path)
{
    const DWORD attrs = GetFileAttributesW(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string NarrowPath(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

bool ReadFileUtf8(const std::wstring& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool WriteFileUtf8Atomic(const std::wstring& path, const std::string& body)
{
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!out)
            return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        // Fall back to overwrite-in-place if replace fails (e.g. cross-volume).
        if (!CopyFileW(tmp.c_str(), path.c_str(), FALSE)) {
            DeleteFileW(tmp.c_str());
            return false;
        }
        DeleteFileW(tmp.c_str());
    }
    return true;
}

// Scan existing <resolution> blocks for width/height/refresh_rate triples.
std::vector<ModeKey> ParseResolutions(const std::string& xml)
{
    std::vector<ModeKey> out;
    size_t pos = 0;
    while (true) {
        const size_t resOpen = xml.find("<resolution>", pos);
        if (resOpen == std::string::npos)
            break;
        const size_t resClose = xml.find("</resolution>", resOpen);
        if (resClose == std::string::npos)
            break;
        const std::string block = xml.substr(resOpen, resClose - resOpen);

        auto tagU32 = [&](const char* openTag, const char* closeTag, uint32_t& v) -> bool {
            const size_t a = block.find(openTag);
            if (a == std::string::npos)
                return false;
            const size_t start = a + strlen(openTag);
            const size_t b = block.find(closeTag, start);
            if (b == std::string::npos)
                return false;
            v = static_cast<uint32_t>(strtoul(block.c_str() + start, nullptr, 10));
            return v > 0;
        };

        ModeKey m;
        if (tagU32("<width>", "</width>", m.w) && tagU32("<height>", "</height>", m.h)) {
            if (!tagU32("<refresh_rate>", "</refresh_rate>", m.hz))
                m.hz = 60;
            out.push_back(m);
        }
        pos = resClose + 13;
    }
    return out;
}

std::string FormatResolutionBlock(uint32_t w, uint32_t h, uint32_t hz)
{
    char buf[192];
    snprintf(buf, sizeof(buf),
             "        <resolution>\n"
             "            <width>%u</width>\n"
             "            <height>%u</height>\n"
             "            <refresh_rate>%u</refresh_rate>\n"
             "        </resolution>\n",
             w, h, hz);
    return buf;
}

// Landscape-only modes @ 60 Hz (and any explicit m.hz); de-dupe.
// Portrait is Windows Display orientation — listing both WxH and HxW fought
// hello vs DXGI and made "Keep" resolution useless under mode forcing.
std::vector<ModeKey> ExpandWanted(const std::vector<MttMode>& wanted)
{
    std::vector<ModeKey> keys;
    auto add = [&](uint32_t w, uint32_t h, uint32_t hz) {
        if (w == 0 || h == 0 || hz == 0)
            return;
        if (h > w)
            std::swap(w, h); // store landscape only
        ModeKey k{w, h, hz};
        if (std::find(keys.begin(), keys.end(), k) == keys.end())
            keys.push_back(k);
    };
    for (const MttMode& m : wanted) {
        const uint32_t hz = m.hz ? m.hz : 60u;
        add(m.width, m.height, hz);
        if (hz != 60u)
            add(m.width, m.height, 60u);
    }
    return keys;
}

// Replace the entire <resolutions>...</resolutions> body with `modes` (prune leftovers).
std::string ReplaceResolutionsXml(const std::string& xml, const std::vector<ModeKey>& modes)
{
    const size_t open = xml.find("<resolutions>");
    const size_t close = xml.rfind("</resolutions>");
    if (open == std::string::npos || close == std::string::npos || close < open)
        return {}; // malformed

    const size_t bodyStart = open + strlen("<resolutions>");
    std::string body = "\n";
    for (const ModeKey& m : modes)
        body += FormatResolutionBlock(m.w, m.h, m.hz);
    body += "    ";

    std::string out = xml;
    out.replace(bodyStart, close - bodyStart, body);
    return out;
}

bool SameModeSet(std::vector<ModeKey> a, std::vector<ModeKey> b)
{
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

// Ask the MttVDD driver to re-read vdd_settings.xml without ChangeDisplaySettings.
// Prefer pnputil /restart-device on Root\MttVDD. Fall back to disable/enable of the
// MTT1337 *monitor* only - never DICS_DISABLE the display adapter (that can leave
// CM_PROB_DISABLED / pending reboot on this class of device).
bool ReloadMttVddDevice(std::string& detail)
{
    auto findMttAdapterInstanceId = [](std::wstring& outId) -> bool {
        HDEVINFO devInfo = SetupDiGetClassDevsW(&kDisplayClassGuid, nullptr, nullptr, DIGCF_PRESENT);
        if (devInfo == INVALID_HANDLE_VALUE)
            return false;
        SP_DEVINFO_DATA did{};
        did.cbSize = sizeof(did);
        bool found = false;
        for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &did); ++i) {
            wchar_t hwIds[1024] = {};
            if (!SetupDiGetDeviceRegistryPropertyW(devInfo, &did, SPDRP_HARDWAREID, nullptr,
                                                   reinterpret_cast<PBYTE>(hwIds), sizeof(hwIds), nullptr))
                continue;
            bool match = false;
            for (const wchar_t* p = hwIds; *p; p += wcslen(p) + 1) {
                if (_wcsicmp(p, L"Root\\MttVDD") == 0) {
                    match = true;
                    break;
                }
            }
            if (!match)
                continue;
            wchar_t instanceId[256];
            if (!SetupDiGetDeviceInstanceIdW(devInfo, &did, instanceId, 256, nullptr))
                continue;
            outId = instanceId;
            found = true;
            break;
        }
        SetupDiDestroyDeviceInfoList(devInfo);
        return found;
    };

    auto runPnputilRestart = [](const std::wstring& instanceId, std::string& detailOut) -> bool {
        std::wstring args = L"/restart-device "" + instanceId + L""";
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"open";
        sei.lpFile = L"pnputil.exe";
        sei.lpParameters = args.c_str();
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr) {
            detailOut = "pnputil launch failed";
            return false;
        }
        WaitForSingleObject(sei.hProcess, 30000);
        DWORD code = 1;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        if (code == 0) {
            detailOut = "reloaded MttVDD via pnputil /restart-device";
            Sleep(1000);
            return true;
        }
        char buf[96];
        snprintf(buf, sizeof(buf), "pnputil /restart-device exit=%lu", static_cast<unsigned long>(code));
        detailOut = buf;
        return false;
    };

    // Monitor-class bounce: DISPLAY\MTT1337\... - softer than killing the adapter.
    auto bounceMttMonitor = [](std::string& detailOut) -> bool {
        static const GUID kMonitorClass = {
            0x4d36e96e, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
        HDEVINFO devInfo = SetupDiGetClassDevsW(&kMonitorClass, nullptr, nullptr, DIGCF_PRESENT);
        if (devInfo == INVALID_HANDLE_VALUE) {
            detailOut = "monitor SetupDiGetClassDevs failed";
            return false;
        }
        SP_DEVINFO_DATA did{};
        did.cbSize = sizeof(did);
        bool ok = false;
        for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &did); ++i) {
            wchar_t instanceId[256];
            if (!SetupDiGetDeviceInstanceIdW(devInfo, &did, instanceId, 256, nullptr))
                continue;
            if (wcsstr(instanceId, L"MTT1337") == nullptr)
                continue;

            auto propChange = [&](DWORD state) -> bool {
                SP_PROPCHANGE_PARAMS pcp{};
                pcp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
                pcp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
                pcp.StateChange = state;
                pcp.Scope = DICS_FLAG_GLOBAL;
                pcp.HwProfile = 0;
                if (!SetupDiSetClassInstallParamsW(devInfo, &did, &pcp.ClassInstallHeader, sizeof(pcp)))
                    return false;
                return SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devInfo, &did) != FALSE;
            };

            const bool disabled = propChange(DICS_DISABLE);
            Sleep(800);
            const bool enabled = propChange(DICS_ENABLE);
            ok = disabled && enabled;
            detailOut = ok ? "reloaded MTT monitor via SetupAPI disable/enable (MTT1337)"
                           : "MTT1337 monitor disable/enable failed";
            break;
        }
        SetupDiDestroyDeviceInfoList(devInfo);
        if (!ok && detailOut.empty())
            detailOut = "MTT1337 monitor not found";
        return ok;
    };

    std::wstring instanceId;
    if (!findMttAdapterInstanceId(instanceId)) {
        detail = "MttVDD root device (Root\\MttVDD) not found";
        return false;
    }

    std::string step;
    if (runPnputilRestart(instanceId, step)) {
        detail = step;
        return true;
    }
    detail = step + "; ";
    if (bounceMttMonitor(step)) {
        detail += step;
        return true;
    }
    detail += step + " (modes written; Windows may need a display re-plug)";
    return false;
}



int ParseMonitorCount(const std::string& xml)
{
    const size_t monOpen = xml.find("<monitors>");
    const size_t monClose = xml.find("</monitors>");
    if (monOpen == std::string::npos || monClose == std::string::npos || monClose < monOpen)
        return -1;
    const std::string block = xml.substr(monOpen, monClose - monOpen);
    const size_t a = block.find("<count>");
    const size_t b = block.find("</count>");
    if (a == std::string::npos || b == std::string::npos || b <= a)
        return -1;
    return static_cast<int>(strtol(block.c_str() + a + strlen("<count>"), nullptr, 10));
}

std::string ReplaceMonitorCountXml(const std::string& xml, uint32_t count)
{
    const size_t monOpen = xml.find("<monitors>");
    const size_t monClose = xml.find("</monitors>");
    if (monOpen == std::string::npos || monClose == std::string::npos || monClose < monOpen)
        return {};
    const size_t blockEnd = monClose + strlen("</monitors>");
    char body[128];
    snprintf(body, sizeof(body),
             "<monitors>\n"
             "        <count>%u</count>\n"
             "    </monitors>",
             count);
    std::string out = xml;
    out.replace(monOpen, blockEnd - monOpen, body);
    return out;
}

bool WaitForMttMonitor(bool wantPresent, int timeoutMs)
{
    ExistingMonitor mon{};
    int waited = 0;
    while (waited < timeoutMs) {
        const bool present = FindMttVirtualMonitor(mon);
        if (present == wantPresent)
            return true;
        Sleep(200);
        waited += 200;
    }
    ExistingMonitor again{};
    return FindMttVirtualMonitor(again) == wantPresent;
}

std::wstring AppDataDirW()
{
    wchar_t* appData = nullptr;
    size_t len = 0;
    std::wstring base;
    if (_wdupenv_s(&appData, &len, L"APPDATA") == 0 && appData) {
        base = appData;
        free(appData);
    }
    if (base.empty())
        base = L".";
    return base + L"\\opendisplay-win";
}

std::wstring MttTopologyPathW()
{
    return AppDataDirW() + L"\\mtt_display.json";
}

bool JsonReadIntField(const std::string& json, const char* key, int& out)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos)
        return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return false;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
        ++pos;
    char* endp = nullptr;
    long v = std::strtol(json.c_str() + pos, &endp, 10);
    if (endp == json.c_str() + pos)
        return false;
    out = static_cast<int>(v);
    return true;
}

bool JsonReadU32Field(const std::string& json, const char* key, uint32_t& out)
{
    int v = 0;
    if (!JsonReadIntField(json, key, v) || v < 0)
        return false;
    out = static_cast<uint32_t>(v);
    return true;
}

bool JsonReadStringField(const std::string& json, const char* key, std::string& out)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos)
        return false;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return false;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    if (pos >= json.size() || json[pos] != '"')
        return false;
    size_t end = json.find('"', pos + 1);
    if (end == std::string::npos)
        return false;
    out.assign(json, pos + 1, end - (pos + 1));
    return true;
}

struct MttTopology {
    int x = 0;
    int y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t hz = 0;
    std::string deviceId;
    bool valid = false;
};

bool LoadTopologyFile(MttTopology& out)
{
    std::string body;
    if (!ReadFileUtf8(MttTopologyPathW(), body) || body.empty())
        return false;
    int x = 0, y = 0;
    if (!JsonReadIntField(body, "x", x) || !JsonReadIntField(body, "y", y))
        return false;
    uint32_t w = 0, h = 0, hz = 0;
    (void)JsonReadU32Field(body, "width", w);
    (void)JsonReadU32Field(body, "height", h);
    (void)JsonReadU32Field(body, "hz", hz);
    std::string id;
    (void)JsonReadStringField(body, "deviceId", id);
    out = {};
    out.x = x;
    out.y = y;
    out.width = w;
    out.height = h;
    out.hz = hz;
    out.deviceId = std::move(id);
    out.valid = true;
    return true;
}

bool WriteTopologyFile(const MttTopology& t)
{
    const std::wstring dir = AppDataDirW();
    CreateDirectoryW(dir.c_str(), nullptr);
    std::ostringstream ss;
    ss << "{\n"
       << "  \"x\": " << t.x << ",\n"
       << "  \"y\": " << t.y << ",\n"
       << "  \"width\": " << t.width << ",\n"
       << "  \"height\": " << t.height << ",\n"
       << "  \"hz\": " << t.hz << ",\n"
       << "  \"deviceId\": \"";
    for (char c : t.deviceId) {
        if (c == '\\')
            ss << "\\\\";
        else if (c != '"')
            ss << c;
    }
    ss << "\"\n}\n";
    return WriteFileUtf8Atomic(MttTopologyPathW(), ss.str());
}

struct MttGraceState {
    std::mutex mu;
    std::condition_variable cv;
    bool pending = false;
    uint64_t epoch = 0;
    std::string logTag;
    std::thread worker;
};

MttGraceState& MttGrace()
{
    static MttGraceState g;
    return g;
}


bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    if (n <= 0)
        return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(n));
    in.read(reinterpret_cast<char*>(out.data()), n);
    return static_cast<bool>(in) || in.eof();
}

bool WriteFileBytesAtomic(const std::wstring& path, const std::vector<uint8_t>& body)
{
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
        if (!out)
            return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (!CopyFileW(tmp.c_str(), path.c_str(), FALSE)) {
            DeleteFileW(tmp.c_str());
            return false;
        }
        DeleteFileW(tmp.c_str());
    }
    return true;
}

bool FilesEqual(const std::wstring& a, const std::wstring& b)
{
    std::vector<uint8_t> ba, bb;
    if (!ReadFileBytes(a, ba) || !ReadFileBytes(b, bb))
        return false;
    return ba == bb;
}

std::wstring ExeDirW()
{
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
        return {};
    std::wstring p(exe);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};
    return p.substr(0, slash);
}

std::wstring FindShippedUserEdid()
{
    const std::wstring exeDir = ExeDirW();
    const std::wstring candidates[] = {
        exeDir + L"\\assets\\mtt\\user_edid.bin",
        exeDir + L"\\user_edid.bin",
        kDevEdidPath,
        kAppsEdidPath,
        kLiveEdidPath,
    };
    for (const std::wstring& c : candidates) {
        if (!c.empty() && FileExists(c.c_str()))
            return c;
    }
    return {};
}


bool ParseOptionBool(const std::string& xml, const char* tag, bool& out)
{
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    const size_t a = xml.find(open);
    if (a == std::string::npos)
        return false;
    const size_t start = a + open.size();
    const size_t b = xml.find(close, start);
    if (b == std::string::npos)
        return false;
    std::string v = xml.substr(start, b - start);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r' || v.front() == '\n'))
        v.erase(v.begin());
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n'))
        v.pop_back();
    if (v == "true" || v == "1") {
        out = true;
        return true;
    }
    if (v == "false" || v == "0") {
        out = false;
        return true;
    }
    return false;
}

std::string SetOptionBoolXml(const std::string& xml, const char* tag, bool value)
{
    const std::string open = std::string("<") + tag + ">";
    const std::string close = std::string("</") + tag + ">";
    const size_t a = xml.find(open);
    if (a == std::string::npos)
        return {};
    const size_t start = a + open.size();
    const size_t b = xml.find(close, start);
    if (b == std::string::npos)
        return {};
    std::string out = xml;
    out.replace(start, b - start, value ? "true" : "false");
    return out;
}



// ---- MTT topology policy state ------------------------------------------
// Serializes attach / restore / save / teardown across sender threads, the
// grace worker and the tray thread. Recursive: Ensure -> Restore, user
// teardown -> Save -> TearDown. Never hold it while joining the grace worker
// (the worker takes it inside TearDown).
std::recursive_mutex& MttTopoMutex()
{
    static std::recursive_mutex mu;
    return mu;
}

// True once the saved position has been applied to the current MTT head
// attachment (real CDS attach by us, or one-time adoption of a head that was
// already on the desktop, e.g. left over from a previous run / reboot). Reset
// when we tear the head down. While true, nothing re-applies x/y: ACCESS_LOST,
// rotate/resize rebuilds and hello-again all leave the user's layout alone.
std::atomic<bool> g_mttPositionedThisAttach{false};

// Set on Quit / session end: EnsureMttVddAttached must not re-attach the head
// from a racing pipeline rebuild while we are tearing it down for exit.
std::atomic<bool> g_mttShuttingDown{false};

// Our own topology applies (attach, restore) stamp NoteSelfDisplayChange();
// observers ignore display changes within this window.
constexpr uint32_t kSelfApplySettleMs = 2000;

struct ActiveDisplayInfo {
    std::wstring gdiName;
    RECT rect{};
    uint32_t sourceId = 0;
    LUID adapter{};
    uint32_t hzNum = 0, hzDen = 0;
};

// Active CCD paths with a valid source mode (desktop rects in physical px).
bool QueryActiveDisplays(std::vector<ActiveDisplayInfo>& out)
{
    out.clear();
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG rc = ERROR_INSUFFICIENT_BUFFER;
    for (int tries = 0; tries < 4 && rc == ERROR_INSUFFICIENT_BUFFER; ++tries) {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS)
            return false;
        paths.resize(np);
        modes.resize(nm);
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr);
        if (rc == ERROR_SUCCESS) {
            paths.resize(np);
            modes.resize(nm);
        }
    }
    if (rc != ERROR_SUCCESS)
        return false;
    for (const auto& p : paths) {
        if (!(p.flags & DISPLAYCONFIG_PATH_ACTIVE))
            continue;
        const UINT32 idx = p.sourceInfo.modeInfoIdx;
        if (idx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || idx >= modes.size() ||
            modes[idx].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
            continue;
        const DISPLAYCONFIG_SOURCE_MODE& sm = modes[idx].sourceMode;
        if (sm.width == 0 || sm.height == 0)
            continue;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = p.sourceInfo.adapterId;
        sn.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS)
            continue;
        ActiveDisplayInfo d;
        d.gdiName = sn.viewGdiDeviceName;
        d.rect.left = sm.position.x;
        d.rect.top = sm.position.y;
        d.rect.right = sm.position.x + static_cast<LONG>(sm.width);
        d.rect.bottom = sm.position.y + static_cast<LONG>(sm.height);
        d.sourceId = p.sourceInfo.id;
        d.adapter = p.sourceInfo.adapterId;
        d.hzNum = p.targetInfo.refreshRate.Numerator;
        d.hzDen = p.targetInfo.refreshRate.Denominator;
        out.push_back(std::move(d));
    }
    return true;
}

bool RectsOverlap(const RECT& a, const RECT& b)
{
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

// MTT head attached AND active in the CCD topology with a valid, non-cloned
// source mode. Fills the live rect. This is the gate for every save: when the
// user picked "Show only on 1" (MTT inactive) or the head is absent / mid
// reconfigure, we must never overwrite the good saved position.
bool QueryMttActiveRect(const std::wstring& gdiName, RECT& rect, std::string* why)
{
    std::vector<ActiveDisplayInfo> act;
    if (!QueryActiveDisplays(act)) {
        if (why)
            *why = "QueryDisplayConfig failed";
        return false;
    }
    const ActiveDisplayInfo* mtt = nullptr;
    for (const auto& d : act) {
        if (_wcsicmp(d.gdiName.c_str(), gdiName.c_str()) == 0) {
            mtt = &d;
            break;
        }
    }
    if (!mtt) {
        if (why)
            *why = "MTT not active in topology";
        return false;
    }
    for (const auto& d : act) {
        if (&d == mtt)
            continue;
        const bool sameSource = d.sourceId == mtt->sourceId && d.adapter.LowPart == mtt->adapter.LowPart &&
                                d.adapter.HighPart == mtt->adapter.HighPart;
        if (sameSource || RectsOverlap(d.rect, mtt->rect)) {
            if (why)
                *why = "MTT cloned/overlapping another display";
            return false;
        }
    }
    rect = mtt->rect;
    return true;
}

// Would placing the MTT head at (x,y) with w x h overlap another active
// display? Then Windows would reshuffle the layout ("position reset").
bool SavedRectOverlapsOthers(const std::wstring& mttGdiName, int x, int y, uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0)
        return false;
    std::vector<ActiveDisplayInfo> act;
    if (!QueryActiveDisplays(act))
        return false;
    RECT want{x, y, x + static_cast<LONG>(w), y + static_cast<LONG>(h)};
    for (const auto& d : act) {
        if (!mttGdiName.empty() && _wcsicmp(d.gdiName.c_str(), mttGdiName.c_str()) == 0)
            continue;
        if (RectsOverlap(d.rect, want))
            return true;
    }
    return false;
}

// Re-commit the current active topology (MTT already detached) to the CCD
// persistence database so Windows does not re-extend the MTT head on the next
// display re-enumeration / reboot. CDS_UPDATEREGISTRY alone only updates the
// legacy registry view.
bool PersistActiveTopologyToDatabase(std::string& detail)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG rc = ERROR_INSUFFICIENT_BUFFER;
    for (int tries = 0; tries < 4 && rc == ERROR_INSUFFICIENT_BUFFER; ++tries) {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) {
            detail = "GetDisplayConfigBufferSizes failed";
            return false;
        }
        paths.resize(np);
        modes.resize(nm);
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr);
        if (rc == ERROR_SUCCESS) {
            paths.resize(np);
            modes.resize(nm);
        }
    }
    if (rc != ERROR_SUCCESS || paths.empty()) {
        detail = "QueryDisplayConfig failed rc=" + std::to_string(rc);
        return false;
    }
    NoteSelfDisplayChange();
    const LONG sr = SetDisplayConfig(static_cast<UINT32>(paths.size()), paths.data(),
                                     static_cast<UINT32>(modes.size()), modes.data(),
                                     SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE |
                                         SDC_ALLOW_CHANGES);
    NoteSelfDisplayChange();
    if (sr != ERROR_SUCCESS) {
        detail = "SetDisplayConfig(SAVE_TO_DATABASE) rc=" + std::to_string(sr);
        return false;
    }
    detail = "topology saved to CCD database";
    return true;
}

} // namespace

std::wstring FindLiveSettingsPath()
{
    if (FileExists(kLiveSettingsPath))
        return kLiveSettingsPath;
    return {};
}

bool MatchesHelloAspect(uint32_t helloW, uint32_t helloH, uint32_t w, uint32_t h)
{
    if (helloW == 0 || helloH == 0 || w == 0 || h == 0)
        return false;
    const double target = static_cast<double>(helloW) / static_cast<double>(helloH);
    const double r1 = static_cast<double>(w) / static_cast<double>(h);
    const double r2 = static_cast<double>(h) / static_cast<double>(w);
    const bool a = std::fabs(r1 - target) / target <= 0.01;
    const bool b = std::fabs(r2 - target) / target <= 0.01;
    return a || b;
}


std::vector<MttMode> BakedMttModes()
{
    // Must match assets/mtt/user_edid.bin DTDs (tools/gen_user_edid.py).
    // Preferred / hello-native first.
    return {
        {2352, 1632, 60},
        {2384, 1664, 60},
        {1760, 1216, 60},
        {1168, 816, 60},
    };
}

MttEnsureResult EnsureCustomEdid()
{
    MttEnsureResult r;
    r.path = FindLiveSettingsPath();
    if (r.path.empty()) {
        r.detail = "live settings missing (expected C:\\VirtualDisplayDriver\\vdd_settings.xml)";
        return r;
    }

    const std::wstring shipped = FindShippedUserEdid();
    if (shipped.empty()) {
        r.detail = "shipped user_edid.bin not found (assets/mtt/user_edid.bin)";
        return r;
    }

    std::string xml;
    if (!ReadFileUtf8(r.path, xml)) {
        r.detail = "failed to read live settings";
        return r;
    }

    bool changed = false;
    std::vector<std::string> notes;

    // 1) Install/refresh live user_edid.bin from shipped asset.
    const bool edidPresent = FileExists(kLiveEdidPath);
    const bool edidSame = edidPresent && FilesEqual(shipped, kLiveEdidPath);
    if (!edidSame) {
        std::vector<uint8_t> bytes;
        if (!ReadFileBytes(shipped, bytes) || bytes.size() < 128 || (bytes.size() % 128) != 0) {
            r.detail = "shipped user_edid.bin invalid size";
            return r;
        }
        if (!WriteFileBytesAtomic(kLiveEdidPath, bytes)) {
            const DWORD err = GetLastError();
            char buf[96];
            snprintf(buf, sizeof(buf), "edid write failed (err=%lu)%s",
                     static_cast<unsigned long>(err),
                     err == ERROR_ACCESS_DENIED ? " access denied" : "");
            r.detail = buf;
            return r;
        }
        if (GetFileAttributesW(L"D:\\apps\\VirtualDisplayDriver") != INVALID_FILE_ATTRIBUTES)
            (void)WriteFileBytesAtomic(kAppsEdidPath, bytes);
        changed = true;
        notes.push_back(edidPresent ? "refreshed user_edid.bin" : "installed user_edid.bin");
    }

    // 2) CustomEdid=true, PreventSpoof=true (stable manufacturer+serial identity).
    bool custom = false, prevent = false;
    const bool haveCustom = ParseOptionBool(xml, "CustomEdid", custom);
    const bool havePrevent = ParseOptionBool(xml, "PreventSpoof", prevent);
    if (!haveCustom || !custom || !havePrevent || !prevent) {
        std::string next = xml;
        if (haveCustom)
            next = SetOptionBoolXml(next, "CustomEdid", true);
        if (next.empty()) {
            r.detail = "malformed XML (no CustomEdid)";
            return r;
        }
        if (havePrevent) {
            const std::string n2 = SetOptionBoolXml(next, "PreventSpoof", true);
            if (n2.empty()) {
                r.detail = "malformed XML (no PreventSpoof)";
                return r;
            }
            next = n2;
        }
        if (!WriteFileUtf8Atomic(r.path, next)) {
            const DWORD err = GetLastError();
            char buf[96];
            snprintf(buf, sizeof(buf), "options write failed (err=%lu)%s",
                     static_cast<unsigned long>(err),
                     err == ERROR_ACCESS_DENIED ? " access denied" : "");
            r.detail = buf;
            return r;
        }
        xml = next;
        changed = true;
        notes.push_back("CustomEdid=true PreventSpoof=true");
    }

    // 3) Mirror baked modes into XML <resolutions> (IddCx mode list source).
    // Do NOT rewrite from hello sizes anymore.
    const std::vector<ModeKey> need = ExpandWanted(BakedMttModes());
    const std::vector<ModeKey> present = ParseResolutions(xml);
    if (!SameModeSet(present, need)) {
        const std::string rewritten = ReplaceResolutionsXml(xml, need);
        if (rewritten.empty()) {
            r.detail = "malformed XML (no <resolutions>)";
            return r;
        }
        if (!WriteFileUtf8Atomic(r.path, rewritten)) {
            const DWORD err = GetLastError();
            char buf[96];
            snprintf(buf, sizeof(buf), "resolutions write failed (err=%lu)%s",
                     static_cast<unsigned long>(err),
                     err == ERROR_ACCESS_DENIED ? " access denied" : "");
            r.detail = buf;
            return r;
        }
        if (FileExists(kAppsSettingsPath) ||
            GetFileAttributesW(L"D:\\apps\\VirtualDisplayDriver") != INVALID_FILE_ATTRIBUTES)
            (void)WriteFileUtf8Atomic(kAppsSettingsPath, rewritten);
        xml = rewritten;
        changed = true;
        r.added = static_cast<int>(need.size());
        notes.push_back("mirrored " + std::to_string(need.size()) + " baked modes into XML");
    }

    if (!changed) {
        r.ok = true;
        r.changed = false;
        r.detail = "CustomEdid already current (bin+flags+baked modes)";
        return r;
    }

    std::string reloadDetail;
    const bool reloaded = ReloadMttVddDevice(reloadDetail);
    r.ok = true;
    r.changed = true;
    r.detail.clear();
    for (size_t i = 0; i < notes.size(); ++i) {
        if (i)
            r.detail += "; ";
        r.detail += notes[i];
    }
    r.detail += "; " + reloadDetail;
    if (!reloaded)
        r.detail += " (reload soft-failed; files written)";
    return r;
}

std::vector<MttMode> BuildIpadModeList(uint32_t helloW, uint32_t helloH)
{
    // Exclusive OpenDisplay MTT head: landscape hello-native aspect (+ scales).
    // Other iPad class sizes kept only when aspect matches within ~1%.
    // No portrait duplicates — Windows Display orientation handles that.
    static const MttMode kCommon[] = {
        {2352, 1632, 60}, // 2360x1640 (11" class)
        {2384, 1664, 60}, // 2388x1668 (newer 11)
        {2720, 2048, 60}, // 2732x2048 (12.9)
        {2256, 1488, 60}, // 2266x1488 (mini)
    };

    if (helloH > helloW)
        std::swap(helloW, helloH);

    std::vector<MttMode> out;
    auto pushUnique = [&](uint32_t w, uint32_t h) {
        if (w == 0 || h == 0)
            return;
        if (h > w)
            std::swap(w, h);
        for (const MttMode& m : out)
            if (m.width == w && m.height == h)
                return;
        out.push_back({w, h, 60});
    };

    auto align16Floor = [](uint32_t v) -> uint32_t { return v & ~15u; };

    pushUnique(helloW, helloH);
    // 3/4 and 1/2 native, 16-aligned, same aspect (within floor error).
    pushUnique(align16Floor(helloW * 3 / 4), align16Floor(helloH * 3 / 4));
    pushUnique(align16Floor(helloW / 2), align16Floor(helloH / 2));

    for (const MttMode& m : kCommon) {
        if (MatchesHelloAspect(helloW, helloH, m.width, m.height))
            pushUnique(m.width, m.height);
    }
    return out;
}

MttEnsureResult EnsureResolutions(const std::vector<MttMode>& wanted)
{
    MttEnsureResult r;
    r.path = FindLiveSettingsPath();
    if (r.path.empty()) {
        r.detail = "live settings missing (expected C:\\VirtualDisplayDriver\\vdd_settings.xml)";
        return r;
    }

    std::string xml;
    if (!ReadFileUtf8(r.path, xml)) {
        r.detail = "failed to read live settings";
        return r;
    }

    const std::vector<ModeKey> present = ParseResolutions(xml);
    const std::vector<ModeKey> need = ExpandWanted(wanted);

    // Exclusive OpenDisplay head: rewrite <resolutions> to the wanted set only
    // (prune 16:9 leftovers / other aspects), not append-only.
    if (SameModeSet(present, need)) {
        r.ok = true;
        r.changed = false;
        r.added = 0;
        r.detail = "already pruned to wanted set";
        return r;
    }

    const std::string rewritten = ReplaceResolutionsXml(xml, need);
    if (rewritten.empty()) {
        r.detail = "malformed XML (no <resolutions>)";
        return r;
    }

    const int removed = static_cast<int>(present.size()) - static_cast<int>(need.size());
    r.added = static_cast<int>(need.size());
    if (!WriteFileUtf8Atomic(r.path, rewritten)) {
        const DWORD err = GetLastError();
        char buf[96];
        snprintf(buf, sizeof(buf), "write failed (err=%lu)%s",
                 static_cast<unsigned long>(err),
                 err == ERROR_ACCESS_DENIED ? " access denied" : "");
        r.detail = buf;
        return r;
    }

    // Best-effort keep the install-source template in sync for future installs.
    if (FileExists(kAppsSettingsPath) || GetFileAttributesW(L"D:\\apps\\VirtualDisplayDriver") != INVALID_FILE_ATTRIBUTES)
        (void)WriteFileUtf8Atomic(kAppsSettingsPath, rewritten);

    std::string reloadDetail;
    const bool reloaded = ReloadMttVddDevice(reloadDetail);

    r.ok = true;
    r.changed = true;
    r.detail = "rewrote " + std::to_string(need.size()) + " modes";
    if (removed > 0)
        r.detail += " (pruned ~" + std::to_string(removed) + " extras)";
    r.detail += "; " + reloadDetail;
    if (!reloaded)
        r.detail += " (reload soft-failed; modes written)";
    return r;

}

bool SelfElevateEnsure(uint32_t helloW, uint32_t helloH)
{
    (void)helloW;
    (void)helloH;
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
        return false;
    // Legacy name still accepted by main; prefer --ensure-mtt-edid.
    std::wstring args = L"--ensure-mtt-edid";

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr)
        return false; // UAC declined or launch failed

    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0;
}

bool EnsureMttResolutionsForHello(uint32_t helloW, uint32_t helloH, const std::string& logTag)
{
    // Hello sizes are no longer written into XML <resolutions>. Modes + stable
    // serial live in user_edid.bin (CustomEdid); XML mirrors the baked set.
    MttEnsureResult r = EnsureCustomEdid();

    if (r.ok && !r.changed) {
        Logf(logTag, "MTT VDD settings: CustomEdid current for hello %ux%u path=%s\n", helloW, helloH,
             NarrowPath(r.path).c_str());
        return true;
    }

    if (!r.ok && r.detail.find("access denied") != std::string::npos) {
        Logf(logTag, "MTT VDD settings: CustomEdid write needs admin, elevating...\n");
        if (!SelfElevateEnsure(helloW, helloH)) {
            Logf(logTag, "MTT VDD settings: elevate failed or UAC declined - CustomEdid may be incomplete\n");
            return false;
        }
        r = EnsureCustomEdid();
        if (r.ok) {
            Logf(logTag, "MTT VDD settings: CustomEdid ensured (via elevate) for hello %ux%u path=%s\n", helloW,
                 helloH, NarrowPath(r.path).c_str());
            if (!r.detail.empty())
                Logf(logTag, "MTT VDD settings: %s\n", r.detail.c_str());
            return true;
        }
        Logf(logTag, "MTT VDD settings: post-elevate CustomEdid incomplete (%s)\n", r.detail.c_str());
        return false;
    }

    if (r.ok) {
        Logf(logTag, "MTT VDD settings: CustomEdid updated for hello %ux%u path=%s\n", helloW, helloH,
             NarrowPath(r.path).c_str());
        if (!r.detail.empty())
            Logf(logTag, "MTT VDD settings: %s\n", r.detail.c_str());
        return true;
    }

    Logf(logTag, "MTT VDD settings: CustomEdid ensure failed (%s)\n", r.detail.c_str());
    return false;
}



int ReadMonitorCount()
{
    const std::wstring path = FindLiveSettingsPath();
    if (path.empty())
        return -1;
    std::string xml;
    if (!ReadFileUtf8(path, xml))
        return -1;
    return ParseMonitorCount(xml);
}

MttEnsureResult SetMonitorCount(uint32_t count)
{
    MttEnsureResult r;
    r.path = FindLiveSettingsPath();
    if (r.path.empty()) {
        r.detail = "live settings missing (expected C:\\VirtualDisplayDriver\\vdd_settings.xml)";
        return r;
    }

    std::string xml;
    if (!ReadFileUtf8(r.path, xml)) {
        r.detail = "failed to read live settings";
        return r;
    }

    const int cur = ParseMonitorCount(xml);
    ExistingMonitor attached{};
    const bool headPresent = FindMttVirtualMonitor(attached);

    if (cur == static_cast<int>(count)) {
        if (count == 0 && !headPresent) {
            r.ok = true;
            r.changed = false;
            r.detail = "already count=0 (no MTT head)";
            return r;
        }
        if (count >= 1 && headPresent) {
            r.ok = true;
            r.changed = false;
            r.detail = "already count=" + std::to_string(count) + " (MTT head present)";
            return r;
        }
    }

    if (cur != static_cast<int>(count)) {
        const std::string rewritten = ReplaceMonitorCountXml(xml, count);
        if (rewritten.empty()) {
            r.detail = "malformed XML (no <monitors>)";
            return r;
        }
        if (!WriteFileUtf8Atomic(r.path, rewritten)) {
            const DWORD err = GetLastError();
            char buf[96];
            snprintf(buf, sizeof(buf), "write failed (err=%lu)%s",
                     static_cast<unsigned long>(err),
                     err == ERROR_ACCESS_DENIED ? " access denied" : "");
            r.detail = buf;
            return r;
        }
        if (FileExists(kAppsSettingsPath) ||
            GetFileAttributesW(L"D:\\apps\\VirtualDisplayDriver") != INVALID_FILE_ATTRIBUTES)
            (void)WriteFileUtf8Atomic(kAppsSettingsPath, rewritten);
    }

    std::string reloadDetail;
    const bool reloaded = ReloadMttVddDevice(reloadDetail);
    bool settled = false;
    if (count == 0) {
        settled = WaitForMttMonitor(/*wantPresent=*/false, 4000);
    } else {
        // Attached OR merely present (CDS-detachable) is enough; Ensure attaches.
        settled = WaitForMttMonitor(/*wantPresent=*/true, 3000);
        if (!settled) {
            ExistingMonitor any{};
            settled = FindMttVirtualMonitorDevice(any, /*requireAttached=*/false);
        }
    }

    r.changed = true;
    r.added = 0;
    char buf[192];
    snprintf(buf, sizeof(buf), "set monitors count=%u (was %d); %s; settle=%s",
             count, cur, reloadDetail.c_str(), settled ? "ok" : "timeout");
    r.detail = buf;
    if (!reloaded)
        r.detail += " (reload soft-failed)";
    r.ok = settled;
    return r;
}

bool SelfElevateSetMonitorCount(uint32_t count)
{
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
        return false;
    std::wstring args = L"--set-mtt-monitor-count " + std::to_wstring(count);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr)
        return false;

    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0;
}

bool EnsureMttVddAttached(const std::string& logTag)
{
    if (g_mttShuttingDown.load()) {
        Logf(logTag, "mtt: exiting - not attaching VDD\n");
        return false;
    }
    // Join any grace worker BEFORE taking the topology lock (it locks in TearDown).
    CancelPendingMttVddTeardown();
    std::lock_guard<std::recursive_mutex> topoLock(MttTopoMutex());
    if (g_mttShuttingDown.load())
        return false;

    ExistingMonitor mon{};
    if (FindMttVirtualMonitor(mon)) {
        // Already on the desktop. Position is applied at most once per
        // attachment: if we have not positioned this head yet (left over from a
        // previous run / reboot, or re-extended outside the app) adopt it and
        // apply the saved layout once. Otherwise (ACCESS_LOST / rotate / resize
        // rebuild, hello-again, link-flap reconnect) leave the user's layout
        // alone - re-applying here is what snapped Display Settings moves back.
        if (!g_mttPositionedThisAttach.exchange(true)) {
            Logf(logTag, "mtt: adopting MTT head already on the desktop (%ls) - applying saved layout once\n",
                 mon.deviceName.c_str());
            (void)RestoreMttDisplayTopology(logTag);
        } else {
            Logf(logTag, "mtt: VDD already attached (%ls) - keeping current layout\n", mon.deviceName.c_str());
        }
        return true;
    }

    // Make sure the driver is willing to expose a head.
    MttEnsureResult r = SetMonitorCount(1);
    if (!r.ok && r.detail.find("access denied") != std::string::npos) {
        Logf(logTag, "mtt: set count=1 needs admin, elevating...\n");
        if (!SelfElevateSetMonitorCount(1)) {
            Logf(logTag, "mtt: elevate failed or UAC declined - VDD may be missing\n");
            return false;
        }
        r = SetMonitorCount(1);
    }

    if (FindMttVirtualMonitor(mon)) {
        Logf(logTag, "mtt: VDD attached (%ls) [%s]\n", mon.deviceName.c_str(), r.detail.c_str());
        g_mttPositionedThisAttach = true;
        (void)RestoreMttDisplayTopology(logTag);
        return true;
    }

    // CDS-detached (or not yet on desktop): find the MTT device and attach it.
    if (FindMttVirtualMonitorDevice(mon, /*requireAttached=*/false)) {
        // Prefer last saved landscape mode; else hello-native default. Restore
        // below reasserts position (and mode) from mtt_display.json.
        uint32_t attachW = 2352, attachH = 1632, attachHz = 60;
        int attachX = 0, attachY = 0;
        bool havePos = false;
        {
            MttTopology saved{};
            if (LoadTopologyFile(saved) && saved.valid && saved.width > 0 && saved.height > 0) {
                attachW = saved.width;
                attachH = saved.height;
                if (saved.hz != 0)
                    attachHz = saved.hz;
                attachX = saved.x;
                attachY = saved.y;
                havePos = true;
            }
        }
        if (havePos && SavedRectOverlapsOthers(mon.deviceName, attachX, attachY, attachW, attachH)) {
            Logf(logTag, "mtt: saved position (%d,%d) overlaps another display - letting Windows place the head\n",
                 attachX, attachY);
            havePos = false;
        }
        Logf(logTag, "mtt: attaching detached head %ls at %ux%u@%u%s\n", mon.deviceName.c_str(),
             attachW, attachH, attachHz, havePos ? " (saved position)" : "");
        if (AttachMonitorToDesktop(mon.deviceName, attachW, attachH, attachHz, havePos, attachX,
                                   attachY) &&
            WaitForMttMonitor(/*wantPresent=*/true, 5000)) {
            if (FindMttVirtualMonitor(mon)) {
                Logf(logTag, "mtt: VDD attached via CDS (%ls)\n", mon.deviceName.c_str());
                g_mttPositionedThisAttach = true;
                (void)RestoreMttDisplayTopology(logTag);
                return true;
            }
        }
    }

    // If a prior teardown DICS_DISABLE'd the MTT1337 monitor, re-enable it.
    {
        static const GUID kMonitorClass = {
            0x4d36e96e, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
        HDEVINFO devInfo = SetupDiGetClassDevsW(&kMonitorClass, nullptr, nullptr, 0);
        bool enabled = false;
        if (devInfo != INVALID_HANDLE_VALUE) {
            SP_DEVINFO_DATA did{};
            did.cbSize = sizeof(did);
            for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &did); ++i) {
                wchar_t instanceId[256];
                if (!SetupDiGetDeviceInstanceIdW(devInfo, &did, instanceId, 256, nullptr))
                    continue;
                if (wcsstr(instanceId, L"MTT1337") == nullptr)
                    continue;
                SP_PROPCHANGE_PARAMS pcp{};
                pcp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
                pcp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
                pcp.StateChange = DICS_ENABLE;
                pcp.Scope = DICS_FLAG_GLOBAL;
                pcp.HwProfile = 0;
                if (SetupDiSetClassInstallParamsW(devInfo, &did, &pcp.ClassInstallHeader, sizeof(pcp)) &&
                    SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devInfo, &did))
                    enabled = true;
            }
            SetupDiDestroyDeviceInfoList(devInfo);
        }
        if (enabled) {
            Logf(logTag, "mtt: re-enabled MTT1337 monitor node\n");
            (void)WaitForMttMonitor(/*wantPresent=*/true, 8000);
            if (FindMttVirtualMonitorDevice(mon, /*requireAttached=*/false)) {
                uint32_t aw = 2352, ah = 1632, ahz = 60;
                int ax = 0, ay = 0;
                bool hp = false;
                MttTopology saved{};
                if (LoadTopologyFile(saved) && saved.valid && saved.width > 0 && saved.height > 0) {
                    aw = saved.width;
                    ah = saved.height;
                    if (saved.hz != 0)
                        ahz = saved.hz;
                    ax = saved.x;
                    ay = saved.y;
                    hp = !SavedRectOverlapsOthers(mon.deviceName, ax, ay, aw, ah);
                }
                (void)AttachMonitorToDesktop(mon.deviceName, aw, ah, ahz, hp, ax, ay);
                (void)WaitForMttMonitor(/*wantPresent=*/true, 5000);
            }
        }
    }

    if (FindMttVirtualMonitor(mon)) {
        Logf(logTag, "mtt: VDD attached (%ls) [%s]\n", mon.deviceName.c_str(), r.detail.c_str());
        g_mttPositionedThisAttach = true;
        (void)RestoreMttDisplayTopology(logTag);
        return true;
    }
    Logf(logTag, "mtt: ensure VDD failed (%s)\n", r.detail.c_str());
    return false;
}
bool TearDownMttVdd(const std::string& logTag)
{
    std::lock_guard<std::recursive_mutex> topoLock(MttTopoMutex());
    ExistingMonitor mon{};
    if (!FindMttVirtualMonitor(mon) && ReadMonitorCount() <= 0) {
        Logf(logTag, "mtt: VDD already down\n");
        return true;
    }

    // Prefer CDS detach: reliably removes the head from Display Settings.
    // Leave monitors/<count> alone so the next hello can CDS-attach without a
    // driver reload (count=0 + pnputil was slow and flaky to reverse).
    if (FindMttVirtualMonitor(mon)) {
        Logf(logTag, "mtt: detaching head %ls from desktop\n", mon.deviceName.c_str());
        if (!DetachMonitorFromDesktop(mon.deviceName))
            Logf(logTag, "mtt: CDS detach request failed on %ls\n", mon.deviceName.c_str());
        (void)WaitForMttMonitor(/*wantPresent=*/false, 4000);
    }

    MttEnsureResult r;
    r.ok = !FindMttVirtualMonitor(mon);
    r.changed = true;
    r.detail = r.ok ? "cds detach" : "cds detach incomplete";

    if (!FindMttVirtualMonitor(mon)) {
        Logf(logTag, "mtt: VDD torn down [%s]\n", r.detail.c_str());
        g_mttPositionedThisAttach = false;
        // Make the detach stick: commit the MTT-less topology to the CCD
        // database, otherwise Windows may re-extend the head on the next
        // display re-enumeration or reboot ("still connected after exit").
        std::string persist;
        if (PersistActiveTopologyToDatabase(persist))
            Logf(logTag, "mtt: %s\n", persist.c_str());
        else
            Logf(logTag, "mtt: persist detached topology failed (%s)\n", persist.c_str());
        return true;
    }

    // Last resort: SetupAPI disable of the MTT1337 monitor node.
    {
        static const GUID kMonitorClass = {
            0x4d36e96e, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
        HDEVINFO devInfo = SetupDiGetClassDevsW(&kMonitorClass, nullptr, nullptr, DIGCF_PRESENT);
        bool disabled = false;
        if (devInfo != INVALID_HANDLE_VALUE) {
            SP_DEVINFO_DATA did{};
            did.cbSize = sizeof(did);
            for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &did); ++i) {
                wchar_t instanceId[256];
                if (!SetupDiGetDeviceInstanceIdW(devInfo, &did, instanceId, 256, nullptr))
                    continue;
                if (wcsstr(instanceId, L"MTT1337") == nullptr)
                    continue;
                SP_PROPCHANGE_PARAMS pcp{};
                pcp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
                pcp.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
                pcp.StateChange = DICS_DISABLE;
                pcp.Scope = DICS_FLAG_GLOBAL;
                pcp.HwProfile = 0;
                if (SetupDiSetClassInstallParamsW(devInfo, &did, &pcp.ClassInstallHeader, sizeof(pcp)) &&
                    SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devInfo, &did))
                    disabled = true;
            }
            SetupDiDestroyDeviceInfoList(devInfo);
        }
        if (disabled && WaitForMttMonitor(/*wantPresent=*/false, 4000)) {
            Logf(logTag, "mtt: VDD torn down via SetupAPI disable [%s]\n", r.detail.c_str());
            g_mttPositionedThisAttach = false;
            return true;
        }
    }

    Logf(logTag, "mtt: teardown failed (still attached) [%s]\n", r.detail.c_str());
    return false;
}

void CancelPendingMttVddTeardown()
{
    MttGraceState& g = MttGrace();
    std::thread toJoin;
    {
        std::lock_guard<std::mutex> lock(g.mu);
        ++g.epoch;
        g.pending = false;
        g.cv.notify_all();
        if (g.worker.joinable())
            toJoin = std::move(g.worker);
    }
    if (toJoin.joinable())
        toJoin.join();
}

void RequestMttVddTeardown(const std::string& logTag, MttTeardownReason reason, int graceMs)
{
    if (graceMs <= 0)
        graceMs = kMttLinkLossGraceMs;
    if (reason == MttTeardownReason::UserInitiated) {
        CancelPendingMttVddTeardown();
        Logf(logTag, "mtt: user-initiated teardown\n");
        (void)SaveMttDisplayTopology(logTag);
        (void)TearDownMttVdd(logTag);
        return;
    }

    MttGraceState& g = MttGrace();
    uint64_t myEpoch = 0;
    {
        std::lock_guard<std::mutex> lock(g.mu);
        ++g.epoch;
        myEpoch = g.epoch;
        g.pending = true;
        g.logTag = logTag;
        g.cv.notify_all();
    }

    std::thread prev;
    {
        std::lock_guard<std::mutex> lock(g.mu);
        if (g.worker.joinable())
            prev = std::move(g.worker);
    }
    if (prev.joinable())
        prev.join();

    {
        std::lock_guard<std::mutex> lock(g.mu);
        if (!g.pending || g.epoch != myEpoch)
            return;
        Logf(logTag, "mtt: link-loss grace %d ms (sticky VDD)\n", graceMs);
        g.worker = std::thread([myEpoch, graceMs]() {
            MttGraceState& gs = MttGrace();
            std::string tag;
            {
                std::unique_lock<std::mutex> lock(gs.mu);
                tag = gs.logTag;
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(graceMs);
                while (gs.pending && gs.epoch == myEpoch) {
                    if (gs.cv.wait_until(lock, deadline) == std::cv_status::timeout)
                        break;
                }
                if (!gs.pending || gs.epoch != myEpoch) {
                    Logf(tag.empty() ? "mtt" : tag, "mtt: link-loss grace cancelled\n");
                    return;
                }
                gs.pending = false;
            }
            Logf(tag.empty() ? "mtt" : tag, "mtt: link-loss grace expired - tearing down VDD\n");
            (void)TearDownMttVdd(tag.empty() ? "mtt" : tag);
        });
    }
}

bool SaveMttDisplayTopology(const std::string& logTag)
{
    std::lock_guard<std::recursive_mutex> topoLock(MttTopoMutex());
    static std::string s_lastSkip; // log each distinct skip reason once (poll runs every 2s)

    ExistingMonitor mon{};
    if (!FindMttVirtualMonitor(mon))
        return false; // absent / detached ("Show only on 1"): keep the good saved layout

    // Only save when the head is attached AND active in the CCD topology with a
    // valid, non-cloned source mode. Anything else (inactive, mid-reconfigure,
    // duplicated) would overwrite a good saved position with junk.
    RECT live{};
    std::string why;
    if (!QueryMttActiveRect(mon.deviceName, live, &why)) {
        if (why != s_lastSkip) {
            Logf(logTag, "mtt: not saving layout (%s)\n", why.c_str());
            s_lastSkip = why;
        }
        return false;
    }
    s_lastSkip.clear();

    const int x = live.left;
    const int y = live.top;
    uint32_t w = static_cast<uint32_t>(live.right - live.left);
    uint32_t h = static_cast<uint32_t>(live.bottom - live.top);
    uint32_t hz = 0;

    MttTopology prev{};
    const bool havePrev = LoadTopologyFile(prev) && prev.valid;

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(mon.deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
        hz = dm.dmDisplayFrequency;
        if (dm.dmDisplayOrientation != DMDO_DEFAULT && havePrev && prev.width && prev.height) {
            // Restore always applies landscape; keep the last landscape mode and
            // only take the new position from a rotated head.
            w = prev.width;
            h = prev.height;
            hz = prev.hz;
        }
    }

    if (havePrev && prev.x == x && prev.y == y && prev.width == w && prev.height == h && prev.hz == hz)
        return true;

    MttTopology t;
    t.x = x;
    t.y = y;
    t.width = w;
    t.height = h;
    t.hz = hz;
    t.deviceId = NarrowPath(mon.deviceId);
    t.valid = true;
    if (!WriteTopologyFile(t)) {
        Logf(logTag, "mtt: failed to save position (%d,%d) %ux%u@%u\n", x, y, w, h, hz);
        return false;
    }
    Logf(logTag, "mtt: saved position (%d,%d) %ux%u@%u\n", x, y, w, h, hz);
    return true;
}

bool SaveMttDisplayTopologyIfUserChange(const std::string& logTag)
{
    // A change that lands right after one of our own applies (attach / restore
    // / mode fix) is ours, not the user's - the streaming poll picks up any
    // genuine user change once the settle window has passed.
    if (RecentSelfDisplayChange(kSelfApplySettleMs))
        return false;
    return SaveMttDisplayTopology(logTag);
}

bool QueryMttSavedMode(uint32_t& width, uint32_t& height, uint32_t& hz)
{
    MttTopology t{};
    if (!LoadTopologyFile(t) || !t.valid || t.width == 0 || t.height == 0)
        return false;
    width = t.width;
    height = t.height;
    hz = t.hz;
    return true;
}

bool RestoreMttDisplayTopology(const std::string& logTag)
{
    std::lock_guard<std::recursive_mutex> topoLock(MttTopoMutex());
    MttTopology t{};
    if (!LoadTopologyFile(t) || !t.valid)
        return false;

    ExistingMonitor mon{};
    if (!FindMttVirtualMonitor(mon))
        return false;

    // FindMttVirtualMonitor matched MTT1337 / VDD by MTT (not DISPLAY index).
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    bool haveDm = EnumDisplaySettingsW(mon.deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm) != 0;

    bool modeOk = true;
    if (t.width > 0 && t.height > 0) {
        const uint32_t applyHz = t.hz ? t.hz : 60u;
        const bool already =
            haveDm && dm.dmPelsWidth == t.width && dm.dmPelsHeight == t.height &&
            (t.hz == 0 || dm.dmDisplayFrequency == static_cast<DWORD>(applyHz)) &&
            dm.dmDisplayOrientation == DMDO_DEFAULT;
        if (already) {
            Logf(logTag, "mtt: restoring saved mode %ux%u@%u (already current)\n", t.width, t.height,
                 applyHz);
        } else if (EnsureMonitorMode(mon.deviceName, t.width, t.height, applyHz)) {
            Logf(logTag, "mtt: restoring saved mode %ux%u@%u\n", t.width, t.height, applyHz);
            haveDm = EnumDisplaySettingsW(mon.deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm) != 0;
        } else {
            Logf(logTag, "mtt: restore saved mode %ux%u@%u failed on %ls\n", t.width, t.height, applyHz,
                 mon.deviceName.c_str());
            modeOk = false;
        }
    }

    bool posOk = true;
    const uint32_t curW = haveDm ? dm.dmPelsWidth : t.width;
    const uint32_t curH = haveDm ? dm.dmPelsHeight : t.height;
    if (haveDm && dm.dmPosition.x == t.x && dm.dmPosition.y == t.y) {
        // already there
    } else if (SavedRectOverlapsOthers(mon.deviceName, t.x, t.y, curW, curH)) {
        // e.g. a stale (0,0) would land on top of the primary and Windows
        // would reshuffle everything - keep Windows' placement instead.
        Logf(logTag, "mtt: saved position (%d,%d) overlaps another display - keeping current placement\n",
             t.x, t.y);
    } else if (!SetMonitorDesktopPosition(mon.deviceName, t.x, t.y)) {
        Logf(logTag, "mtt: restore position (%d,%d) failed on %ls\n", t.x, t.y, mon.deviceName.c_str());
        posOk = false;
    } else {
        Logf(logTag, "mtt: restored position (%d,%d)\n", t.x, t.y);
    }
    return modeOk && posOk;
}

void PollMttDisplayTopology(const std::string& logTag, POINT& lastObserved)
{
    // Save-only observer: never restores. SaveMttDisplayTopology no-ops when
    // nothing changed vs mtt_display.json and refuses inactive/absent heads.
    if (SaveMttDisplayTopologyIfUserChange(logTag)) {
        ExistingMonitor mon{};
        RECT r{};
        if (FindMttVirtualMonitor(mon) && QueryMttActiveRect(mon.deviceName, r, nullptr))
            lastObserved = {r.left, r.top};
    }
}

void BeginMttShutdown()
{
    g_mttShuttingDown = true;
}

bool ShutdownMttVddForExit(const std::string& logTag)
{
    g_mttShuttingDown = true;
    CancelPendingMttVddTeardown(); // outside the topology lock (joins grace worker)
    std::lock_guard<std::recursive_mutex> topoLock(MttTopoMutex());
    ExistingMonitor mon{};
    if (!FindMttVirtualMonitor(mon)) {
        Logf(logTag, "mtt: exit - no MTT head on the desktop\n");
        return true;
    }
    Logf(logTag, "mtt: exit - tearing down MTT head %ls\n", mon.deviceName.c_str());
    (void)SaveMttDisplayTopology(logTag);
    return TearDownMttVdd(logTag);
}

void AdoptStaleMttHeadAtStartup(const std::string& logTag, int graceMs)
{
    ExistingMonitor mon{};
    if (!FindMttVirtualMonitor(mon))
        return;
    Logf(logTag,
         "mtt: MTT head %ls already on the desktop at startup (left over from a previous run) - "
         "adopting; removed in %d ms unless an iPad connects\n",
         mon.deviceName.c_str(), graceMs);
    g_mttPositionedThisAttach = false; // first Ensure applies the saved layout once
    RequestMttVddTeardown(logTag, MttTeardownReason::LinkLoss, graceMs);
}

} // namespace od
