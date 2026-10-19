// msdisp-portrait — save / restore the desktop icon layout. Adding, removing or resizing our virtual monitor makes
// Explorer reshuffle desktop icons; this puts them back where the user had them.
#pragma once
#include <string>

namespace msdisp {

struct DesktopIconResult {
    bool ok = false;
    int total = 0;    // icons on the desktop
    int placed = 0;   // icons moved to their saved spot
    int added = 0;    // icons not in the saved layout, placed by the rule (programs left, folders right)
    std::wstring error;
};

// Positions are stored per icon name, relative to the primary monitor's top-left, in
// %APPDATA%\msdisp\desktop-layout.txt (UTF-8: name<TAB>dx<TAB>dy).
std::wstring defaultDesktopLayoutPath();
DesktopIconResult saveDesktopLayout(const std::wstring& path);
// Turns auto-arrange off (keeps snap to grid) and moves every icon to its saved position.
DesktopIconResult restoreDesktopLayout(const std::wstring& path);
// Turns Explorer's auto-arrange on (packs icons into columns). Used by tests.
bool autoArrangeDesktop();

}  // namespace msdisp
