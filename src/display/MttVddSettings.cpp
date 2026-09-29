#include "display/MttVddSettings.h"

#include "app/Log.h"

#include <windows.h>

#include <cfgmgr32.h>
#include <setupapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace od {

namespace {

constexpr wchar_t kLiveSettingsPath[] = L"C:\\VirtualDisplayDriver\\vdd_settings.xml";
constexpr wchar_t kAppsSettingsPath[] = L"D:\\apps\\VirtualDisplayDriver\\vdd_settings.intel.xml";

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

// Expand logical sizes with rotation @ 60 Hz (and any explicit m.hz); de-dupe.
// 30 Hz dropped - OpenDisplay targets 60 on this exclusive MTT head.
std::vector<ModeKey> ExpandWanted(const std::vector<MttMode>& wanted)
{
    std::vector<ModeKey> keys;
    auto add = [&](uint32_t w, uint32_t h, uint32_t hz) {
        if (w == 0 || h == 0 || hz == 0)
            return;
        ModeKey k{w, h, hz};
        if (std::find(keys.begin(), keys.end(), k) == keys.end())
            keys.push_back(k);
    };
    for (const MttMode& m : wanted) {
        const uint32_t hz = m.hz ? m.hz : 60u;
        add(m.width, m.height, hz);
        if (m.width != m.height)
            add(m.height, m.width, hz);
        if (hz != 60u) {
            add(m.width, m.height, 60u);
            if (m.width != m.height)
                add(m.height, m.width, 60u);
        }
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
        std::wstring args = L"/restart-device \"" + instanceId + L"\"";
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

std::vector<MttMode> BuildIpadModeList(uint32_t helloW, uint32_t helloH)
{
    // Exclusive OpenDisplay MTT head: only hello-native aspect (+ same-ratio scales).
    // Other iPad class sizes are kept only when they match hello aspect within ~1%.
    // Rotations added in ExpandWanted. No 16:9 leftovers (1920x1080 / 2560x1440).
    static const MttMode kCommon[] = {
        {2352, 1632, 60}, // 2360x1640 (11" class)
        {2384, 1664, 60}, // 2388x1668 (newer 11)
        {2720, 2048, 60}, // 2732x2048 (12.9)
        {2256, 1488, 60}, // 2266x1488 (mini)
    };

    std::vector<MttMode> out;
    auto pushUnique = [&](uint32_t w, uint32_t h) {
        if (w == 0 || h == 0)
            return;
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
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
        return false;
    std::wstring args = L"--ensure-mtt-resolutions " + std::to_wstring(helloW) + L" " +
                        std::to_wstring(helloH);

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
    const std::vector<MttMode> list = BuildIpadModeList(helloW, helloH);
    MttEnsureResult r = EnsureResolutions(list);

    if (r.ok && !r.changed) {
        Logf(logTag, "MTT VDD settings: ensured %ux%u@60 (added 0 modes) path=%s\n", helloW, helloH,
             NarrowPath(r.path).c_str());
        return true;
    }

    if (!r.ok && r.detail.find("access denied") != std::string::npos) {
        Logf(logTag, "MTT VDD settings: write needs admin, elevating...\n");
        if (!SelfElevateEnsure(helloW, helloH)) {
            Logf(logTag, "MTT VDD settings: elevate failed or UAC declined - modes may be incomplete\n");
            return false;
        }
        // Re-check after elevated child wrote + reloaded (added count is 0 on no-op re-read).
        r = EnsureResolutions(list);
        if (r.ok) {
            Logf(logTag, "MTT VDD settings: ensured %ux%u@60 (via elevate) path=%s\n", helloW, helloH,
                 NarrowPath(r.path).c_str());
            return true;
        }
        Logf(logTag, "MTT VDD settings: post-elevate still incomplete (%s)\n", r.detail.c_str());
        return false;
    }

    if (r.ok) {
        Logf(logTag, "MTT VDD settings: ensured %ux%u@60 (added %d modes) path=%s\n", helloW, helloH, r.added,
             NarrowPath(r.path).c_str());
        if (!r.detail.empty())
            Logf(logTag, "MTT VDD settings: %s\n", r.detail.c_str());
        return true;
    }

    Logf(logTag, "MTT VDD settings: ensure failed (%s)\n", r.detail.c_str());
    return false;
}

} // namespace od
