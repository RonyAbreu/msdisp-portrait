// msdisp-portrait — Parsec VDD internals shared with tools/vddtest (not part of the frozen contract).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace msdisp::vdd {

// Values verified against nomi-san/parsec-vdd core/parsec-vdd.h (driver 0.45).
// CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800 + n, METHOD_BUFFERED, access)
constexpr uint32_t kIoctlAdd = 0x0022e004;      // n=1, FILE_READ_ACCESS | FILE_WRITE_ACCESS
constexpr uint32_t kIoctlRemove = 0x0022a008;   // n=2, FILE_WRITE_ACCESS; input = 16-bit big-endian index
constexpr uint32_t kIoctlUpdate = 0x0022a00c;   // n=3, FILE_WRITE_ACCESS; keep-alive, must be < 1 s apart
constexpr uint32_t kIoctlVersion = 0x0022e010;  // n=4, FILE_READ_ACCESS | FILE_WRITE_ACCESS; returns minor version

// One display path whose monitor is a Parsec virtual display (monitor id "PSCCDD0").
struct ParsecTarget {
    std::wstring gdiName;      // \\.\DISPLAYn of the source driving it (empty if inactive)
    std::wstring monitorPath;  // monitor device interface path, unique per virtual display
    uint32_t targetId = 0;
    uint32_t connector = 0;    // DISPLAYCONFIG_TARGET_DEVICE_NAME.connectorInstance
    bool active = false;
};

// Lists Parsec virtual display targets via QueryDisplayConfig (activeOnly => QDC_ONLY_ACTIVE_PATHS).
std::vector<ParsecTarget> listParsecTargets(bool activeOnly);

}  // namespace msdisp::vdd
