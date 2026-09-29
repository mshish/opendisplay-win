#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace od {

// Encode mode / preset strings match config.json flat keys (usbEncodeMode, ...).
enum class EncodeMode { Dynamic, Fixed };
enum class EncodePreset { Speed, Balanced, Quality };

struct Config {
    // iPad addresses, in menu order; one sender (and one virtual monitor) per
    // entry. Empty until the user configures a device. A config from the
    // single-iPad version (a bare "ip") loads as a one-entry list.
    std::vector<std::string> devices;
    uint16_t port = 9000;         // the iPad receiver's fixed listen port
    bool autoReconnect = true;    // start streaming automatically on launch

    // Per-transport encode prefs (flat JSON). Defaults match today's behavior:
    // Dynamic + Speed (= USB 50 Mbps / Wi-Fi 25 Mbps peak, QVs=0).
    std::string usbEncodeMode = "dynamic";     // "dynamic" | "fixed"
    std::string usbEncodePreset = "speed";     // "speed" | "balanced" | "quality"
    std::string wifiEncodeMode = "dynamic";
    std::string wifiEncodePreset = "speed";

    // Loads the saved config; returns defaults if the file is missing/invalid.
    static Config Load();

    // Writes the config (creating the directory if needed). Best-effort.
    void Save() const;

    // Full path to the config file (also used to derive the app data dir).
    static std::wstring FilePath();
};

inline EncodeMode ParseEncodeMode(const std::string& s)
{
    return (s == "fixed") ? EncodeMode::Fixed : EncodeMode::Dynamic;
}

inline EncodePreset ParseEncodePreset(const std::string& s)
{
    if (s == "balanced")
        return EncodePreset::Balanced;
    if (s == "quality")
        return EncodePreset::Quality;
    return EncodePreset::Speed;
}

inline const char* EncodeModeName(EncodeMode m)
{
    return m == EncodeMode::Fixed ? "fixed" : "dynamic";
}

inline const char* EncodePresetName(EncodePreset p)
{
    switch (p) {
        case EncodePreset::Balanced: return "balanced";
        case EncodePreset::Quality: return "quality";
        default: return "speed";
    }
}

} // namespace od
