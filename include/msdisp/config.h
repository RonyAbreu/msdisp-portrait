// msdisp-portrait — user configuration (INI). FROZEN CONTRACT.
// Owner: track D (src/app).
#pragma once
#include <string>

#include "msdisp/common.h"

namespace msdisp {

struct Config {
    Rotation rotation = Rotation::CW90;  // applied to the portrait virtual desktop
    Mode mode;                           // chip output mode, default 1920x1080@60
    int virtualWidth = 1080;             // virtual monitor size (portrait)
    int virtualHeight = 1920;
    int fpsCap = 60;                     // upper bound on frame submissions
    int tileW = 64, tileH = 8;
    bool stopWinUsbDisplay = true;       // stop vendor app while running
    bool showOverlay = false;            // metrics overlay
    std::wstring logFile;                // empty = %APPDATA%\msdisp\msdisp.log
};

// Missing file/keys => defaults. Returns false only on parse error.
bool loadConfig(const std::wstring& path, Config& out);
bool saveConfig(const std::wstring& path, const Config& cfg);
std::wstring defaultConfigPath();  // %APPDATA%\msdisp\msdisp.ini

}  // namespace msdisp
