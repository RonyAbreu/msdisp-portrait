// desktopicons: save / restore the desktop icon layout from the command line.
//   desktopicons save [file]   |   desktopicons restore [file]   |   desktopicons scramble (test: auto-arrange on)
#include <windows.h>

#include <cstdio>
#include <string>

#include "../../src/app/core/desktop_icons.h"

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: desktopicons save|restore [file]\n");
        return 2;
    }
    if (std::wstring(argv[1]) == L"scramble") return msdisp::autoArrangeDesktop() ? 0 : 1;
    std::wstring cmd = argv[1], path = argc > 2 ? argv[2] : msdisp::defaultDesktopLayoutPath();
    msdisp::DesktopIconResult r = cmd == L"save" ? msdisp::saveDesktopLayout(path) : msdisp::restoreDesktopLayout(path);
    wprintf(L"%ls: ok=%d total=%d placed=%d added=%d %ls\n", cmd.c_str(), r.ok, r.total, r.placed, r.added,
            r.error.c_str());
    return r.ok ? 0 : 1;
}
