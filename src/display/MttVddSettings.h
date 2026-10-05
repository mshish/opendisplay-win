#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace od {

// MTT Virtual Display Driver live config lives at
// C:\VirtualDisplayDriver\{vdd_settings.xml,user_edid.bin}.
// Mode list + stable monitor identity come from a shipped user_edid.bin
// (CustomEdid=true). XML <resolutions> is kept mirrored to the same baked
// landscape set because stock MTT still feeds IddCx QueryTargetModes from XML
// (CustomEdid alone does not emulate resolutions). Writing the live path +
// reloading MttVDD needs admin; the self-elevate one-shot covers that.

struct MttMode {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t hz = 60;
};

struct MttEnsureResult {
    bool ok = false;       // settings readable / modes present (or write+reload succeeded)
    bool changed = false;  // XML and/or EDID was rewritten (reload ran)
    int added = 0;         // number of <resolution> entries in the rewritten set
    std::wstring path;     // live settings path used
    std::string detail;    // short status for logging
};

// Prefer C:\VirtualDisplayDriver\vdd_settings.xml when present.
std::wstring FindLiveSettingsPath();

// Fixed landscape modes baked into assets/mtt/user_edid.bin (and mirrored in XML).
// Preferred / native first. Regenerated via tools/gen_user_edid.py.
std::vector<MttMode> BakedMttModes();

// Hello native size + same-aspect scales (1/2, 3/4) and matching iPad-class
// sizes (~1% aspect). Kept for diagnostics; ensure path no longer rewrites XML
// from hello (uses BakedMttModes + CustomEdid instead).
std::vector<MttMode> BuildIpadModeList(uint32_t helloW, uint32_t helloH);

// Install/refresh C:\VirtualDisplayDriver\user_edid.bin from the shipped asset,
// set CustomEdid=true + PreventSpoof=true, mirror BakedMttModes into XML
// <resolutions>, reload MttVDD. Idempotent when already current.
MttEnsureResult EnsureCustomEdid();

// Rewrite <resolutions> to the wanted set only (prune other aspects) without
// touching gpu/options/monitors. Prefer EnsureCustomEdid for the sender path.
// Needs admin to write the live path; returns ok=false with detail on denial.
MttEnsureResult EnsureResolutions(const std::vector<MttMode>& wanted);

// Elevated one-shot used by --ensure-mtt-resolutions / --ensure-mtt-edid.
bool SelfElevateEnsure(uint32_t helloW, uint32_t helloH);

// Sender entry: EnsureCustomEdid (shipped bin + CustomEdid + baked XML modes).
// helloW/H kept for log context only. UAC decline -> log and return false.
bool EnsureMttResolutionsForHello(uint32_t helloW, uint32_t helloH, const std::string& logTag);

// Read <monitors><count> from live settings. Returns -1 if missing/unreadable.
int ReadMonitorCount();

// Write <monitors><count>N</count> and reload MttVDD. Idempotent when already N.
// Needs admin to write the live path.
MttEnsureResult SetMonitorCount(uint32_t count);

// Elevated one-shot used by --set-mtt-monitor-count.
bool SelfElevateSetMonitorCount(uint32_t count);

// Ensure at least one MTT head is attached (set count>=1 + reload if needed).
// Logs via Logf. Cancels any pending link-loss grace teardown.
// Position policy: the saved layout is applied once per attachment - after a
// fresh attach by us, or once when adopting a head that was already on the
// desktop (previous run / reboot). When the head is already attached and was
// positioned, this is a no-op for layout (ACCESS_LOST / rotate / resize
// rebuilds must never move the monitor back). Refuses after BeginMttShutdown.
bool EnsureMttVddAttached(const std::string& logTag);

// Drop MTT heads via CDS detach. Prefer RequestMttVddTeardown so link-loss
// flaps keep the head sticky across USB<->Wi-Fi.
bool TearDownMttVdd(const std::string& logTag);

// How long after link loss (socket drop / transport flap) we keep the MTT head
// attached waiting for a client hello. Cancelled by EnsureMttVddAttached.
constexpr int kMttLinkLossGraceMs = 6000;

enum class MttTeardownReason {
    UserInitiated, // tray Disconnect / Exit / Stop / app quit
    LinkLoss,      // socket drop, usbmux fail, transport switch, pipeline fail
};

// UserInitiated: cancel grace and TearDown now.
// LinkLoss: start/restart the grace timer; TearDown only if it expires with
// no intervening Ensure/Cancel (client did not return).
// graceMs <= 0 uses kMttLinkLossGraceMs.
void RequestMttVddTeardown(const std::string& logTag, MttTeardownReason reason, int graceMs = 0);

// Cancel a pending link-loss grace teardown (client returned).
void CancelPendingMttVddTeardown();

// Persist the MTT head's current CCD position/mode under
// %APPDATA%\opendisplay-win\mtt_display.json, keyed by MTT1337 identity (not
// \\.\DISPLAYn). Logs "mtt: saved position (x,y) WxH@hz".
// Only saves when the MTT head is attached AND active in the CCD topology with
// a valid, non-cloned source mode; otherwise (inactive / "Show only on 1",
// absent, mid-reconfigure) returns false and leaves the file untouched.
bool SaveMttDisplayTopology(const std::string& logTag);

// Same, but skipped within a short settle window after one of our own display
// applies (see NoteSelfDisplayChange) so our applies are not treated as user
// layout changes. Used by ACCESS_LOST, WM_DISPLAYCHANGE and the streaming poll.
bool SaveMttDisplayTopologyIfUserChange(const std::string& logTag);

// True when mtt_display.json has a usable width x height (hz optional; 0 -> caller
// picks a default). Does not touch the desktop.
bool QueryMttSavedMode(uint32_t& width, uint32_t& height, uint32_t& hz);

// After CDS attach: find the MTT head by identity and reapply saved x,y and
// mode (WxH@hz) when available. No-op when nothing saved yet. Logs
// "mtt: restored position (x,y)" / "mtt: restoring saved mode WxH@hz".
bool RestoreMttDisplayTopology(const std::string& logTag);

// Save-only observer (never restores): persists Display Settings drags /
// resolution picks while streaming so they survive the next reconnect / run.
void PollMttDisplayTopology(const std::string& logTag, POINT& lastObserved);

// Quit / session end: stop any later EnsureMttVddAttached from re-attaching.
void BeginMttShutdown();

// Quit / WM_ENDSESSION / normal process exit: cancel grace, save layout (if
// active), tear the MTT head down and persist the detached topology. Safe to
// call repeatedly; implies BeginMttShutdown.
bool ShutdownMttVddForExit(const std::string& logTag);

// Tray startup: if an MTT head is already on the desktop (left over from a
// previous run / reboot), adopt it - the first Ensure applies the saved layout
// once - and arm a grace teardown so it does not linger without an iPad.
void AdoptStaleMttHeadAtStartup(const std::string& logTag, int graceMs);

} // namespace od
