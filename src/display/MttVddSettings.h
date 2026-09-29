#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace od {

// Merge iPad panel modes into the live MTT Virtual Display Driver settings XML
// (C:\VirtualDisplayDriver\vdd_settings.xml) so Windows Display Settings lists
// them on the MTT monitor. Does NOT ChangeDisplaySettings to force a mode -
// the user picks in Display Settings. Writing the live path + restarting the
// MttVDD device needs admin; the self-elevate one-shot covers that.

struct MttMode {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t hz = 60;
};

struct MttEnsureResult {
    bool ok = false;       // settings readable / modes present (or write+reload succeeded)
    bool changed = false;  // XML was rewritten (reload ran)
    int added = 0;         // number of <resolution> entries in the rewritten set
    std::wstring path;     // live settings path used
    std::string detail;    // short status for logging
};

// Prefer C:\VirtualDisplayDriver\vdd_settings.xml when present.
std::wstring FindLiveSettingsPath();

// Hello native size + same-aspect scales (1/2, 3/4) and matching iPad-class
// sizes (~1% aspect). Ensure expands each with rotation @ 60 Hz.
std::vector<MttMode> BuildIpadModeList(uint32_t helloW, uint32_t helloH);

// Rewrite <resolutions> to the wanted set only (prune other aspects) without
// touching gpu/options/monitors. Idempotent when the set already matches.
// Needs admin to write the live path; returns ok=false with detail on denial.
MttEnsureResult EnsureResolutions(const std::vector<MttMode>& wanted);

// Elevated one-shot used by --ensure-mtt-resolutions and SelfElevateEnsure.
bool SelfElevateEnsure(uint32_t helloW, uint32_t helloH);

// Sender entry: build common+hello list, Ensure; on Access Denied self-elevate.
// Logs via Logf. UAC decline -> log and return false (caller continues).
bool EnsureMttResolutionsForHello(uint32_t helloW, uint32_t helloH, const std::string& logTag);

} // namespace od
