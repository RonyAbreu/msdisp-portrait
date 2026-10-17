// msdisp-portrait — metrics overlay: top-most, click-through layered window (GDI text). Track D.
#pragma once
#include <windows.h>

#include <string>

namespace msdisp {

bool overlayCreate(HINSTANCE inst);  // call once on the UI thread
void overlayDestroy();
void overlayShow(bool show);              // UI thread
void overlaySetText(const std::wstring& text);  // UI thread; multi-line allowed
void overlayReposition();                 // UI thread; after WM_DISPLAYCHANGE

// Monitor the overlay sits on, by GDI device name (MONITORINFOEXW::szDevice, e.g. L"\\\\.\\DISPLAY3").
// Empty or unknown name = primary monitor. Thread-safe; may be called before overlayCreate().
void setOverlayMonitor(const std::wstring& gdiName);

}  // namespace msdisp
