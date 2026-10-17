// msdisp-portrait — virtual monitor (Parsec Virtual Display Driver). FROZEN CONTRACT.
// Owner: track B (src/vdd).
#pragma once
#include <memory>
#include <string>

namespace msdisp {

class IVirtualDisplay {
public:
    virtual ~IVirtualDisplay() = default;
    virtual bool open() = 0;  // opens the Parsec VDD adapter device
    virtual void close() = 0;
    // Adds a display; returns its index (>=0) or -1. Starts the internal keep-alive thread.
    virtual int add() = 0;
    virtual bool remove(int index) = 0;
    // GDI name (\\.\DISPLAYn) of a display we added, empty if not (yet) attached.
    virtual std::wstring gdiName(int index) = 0;
};

std::unique_ptr<IVirtualDisplay> createParsecVdd();

// Writes HKLM\SOFTWARE\Parsec\vdd\<slot> {width,height,hz}. Needs admin. Returns false otherwise.
bool parsecVddSetCustomMode(int slot, int width, int height, int hz);

// Sets the display mode (and orientation) of a GDI output via ChangeDisplaySettingsEx.
bool setDisplayMode(const std::wstring& gdiName, int width, int height, int hz);

}  // namespace msdisp
