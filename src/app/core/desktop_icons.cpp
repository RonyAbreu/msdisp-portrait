#include "desktop_icons.h"

// clang-format off
#include <windows.h>
#include <exdisp.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shobjidl.h>
// clang-format on

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "msdisp/log.h"
#include "util.h"

namespace msdisp {

namespace {

template <typename T>
struct Com {
    T* p = nullptr;
    ~Com() {
        if (p) p->Release();
    }
    T** put() { return &p; }
    T* operator->() const { return p; }
};

struct Desktop {
    Com<IFolderView2> view;
    POINT origin{};    // screen position of the icon coordinate space's (0,0)
    RECT primary{};    // primary monitor rect, screen coordinates
    POINT spacing{76, 101};
};

// Desktop folder view: ShellWindows -> desktop dispatch -> top-level browser -> active view.
bool openDesktop(Desktop& d, std::wstring& err) {
    Com<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(windows.put())))) {
        err = L"Explorer não respondeu (ShellWindows)";
        return false;
    }
    VARIANT loc, root;
    VariantInit(&loc);
    VariantInit(&root);
    loc.vt = VT_I4;
    loc.lVal = CSIDL_DESKTOP;
    long hwnd = 0;
    Com<IDispatch> disp;
    if (windows->FindWindowSW(&loc, &root, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, disp.put()) != S_OK || !disp.p) {
        err = L"área de trabalho não encontrada";
        return false;
    }
    Com<IServiceProvider> sp;
    Com<IShellBrowser> browser;
    Com<IShellView> view;
    if (FAILED(disp->QueryInterface(IID_PPV_ARGS(sp.put()))) ||
        FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(browser.put()))) ||
        FAILED(browser->QueryActiveShellView(view.put())) || FAILED(view->QueryInterface(IID_PPV_ARGS(d.view.put())))) {
        err = L"não foi possível acessar os ícones da área de trabalho";
        return false;
    }
    // Icon coordinates are client coordinates of the desktop list view, which spans all monitors (its origin is
    // the left-most monitor, not the primary one).
    HWND defView = nullptr, list = nullptr;
    view->GetWindow(&defView);
    if (defView) list = FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
    d.origin = POINT{0, 0};
    ClientToScreen(list ? list : defView, &d.origin);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    d.primary = mi.rcWork;
    d.view->GetSpacing(&d.spacing);
    return true;
}

std::wstring nameOf(PCUITEMID_CHILD pidl, SIGDN kind) {
    PWSTR s = nullptr;
    if (FAILED(SHGetNameFromIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(pidl), kind, &s)) || !s) return L"";
    std::wstring r = s;
    CoTaskMemFree(s);
    return r;
}

bool isDirectory(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Folders and shortcuts to folders go right; everything else (apps, shortcuts, files, shell items) left.
bool isFolderLike(PCUITEMID_CHILD pidl) {
    std::wstring path = nameOf(pidl, SIGDN_FILESYSPATH);
    if (path.empty()) return false;
    if (isDirectory(path)) return true;
    if (path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 4, L".lnk") == 0) {
        Com<IShellLinkW> link;
        Com<IPersistFile> file;
        wchar_t target[MAX_PATH] = {};
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put()))) &&
            SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(file.put()))) && SUCCEEDED(file->Load(path.c_str(), STGM_READ)) &&
            SUCCEEDED(link->GetPath(target, MAX_PATH, nullptr, 0)))
            return target[0] && isDirectory(target);
    }
    return false;
}

struct Item {
    PITEMID_CHILD pidl = nullptr;
    std::wstring name;
    POINT screen{};
};

std::vector<Item> listItems(Desktop& d) {
    std::vector<Item> items;
    int n = 0;
    d.view->ItemCount(SVGIO_ALLVIEW, &n);
    for (int i = 0; i < n; ++i) {
        Item it;
        if (FAILED(d.view->Item(i, &it.pidl))) continue;
        POINT pt{};
        d.view->GetItemPosition(it.pidl, &pt);
        it.screen = POINT{pt.x + d.origin.x, pt.y + d.origin.y};
        it.name = nameOf(it.pidl, SIGDN_NORMALDISPLAY);
        items.push_back(std::move(it));
    }
    return items;
}

void freeItems(std::vector<Item>& items) {
    for (auto& it : items) CoTaskMemFree(it.pidl);
    items.clear();
}

struct ComInit {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComInit() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

}  // namespace

std::wstring defaultDesktopLayoutPath() {
    std::wstring dir = appDataDir();
    ensureDirectory(dir);
    return dir + L"\\desktop-layout.txt";
}

DesktopIconResult saveDesktopLayout(const std::wstring& path) {
    ComInit com;
    DesktopIconResult r;
    Desktop d;
    if (!openDesktop(d, r.error)) return r;
    std::vector<Item> items = listItems(d);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        r.error = L"não foi possível gravar " + path;
        freeItems(items);
        return r;
    }
    for (const Item& it : items) {
        std::string line = toUtf8(it.name);
        char nums[64];
        snprintf(nums, sizeof(nums), "\t%ld\t%ld\n", it.screen.x - d.primary.left, it.screen.y - d.primary.top);
        line += nums;
        fwrite(line.data(), 1, line.size(), f);
    }
    fclose(f);
    r.ok = true;
    r.total = r.placed = int(items.size());
    MSD_INFO("desktop: saved %d icon positions to %s", r.total, toUtf8(path).c_str());
    freeItems(items);
    return r;
}

bool autoArrangeDesktop() {
    ComInit com;
    Desktop d;
    std::wstring err;
    if (!openDesktop(d, err)) return false;
    return SUCCEEDED(d.view->SetCurrentFolderFlags(FWF_AUTOARRANGE | FWF_SNAPTOGRID, FWF_AUTOARRANGE | FWF_SNAPTOGRID));
}

DesktopIconResult restoreDesktopLayout(const std::wstring& path) {
    ComInit com;
    DesktopIconResult r;
    std::map<std::wstring, POINT> saved;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) {
        char buf[1024];
        while (fgets(buf, sizeof(buf), f)) {
            char* t1 = strchr(buf, '\t');
            if (!t1) continue;
            *t1 = 0;
            long dx = 0, dy = 0;
            if (sscanf(t1 + 1, "%ld\t%ld", &dx, &dy) == 2) saved[fromUtf8(buf)] = POINT{dx, dy};
        }
        fclose(f);
    }
    Desktop d;
    if (!openDesktop(d, r.error)) return r;
    d.view->SetCurrentFolderFlags(FWF_AUTOARRANGE | FWF_SNAPTOGRID, FWF_SNAPTOGRID);
    std::vector<Item> items = listItems(d);
    r.total = int(items.size());

    const LONG cellW = d.spacing.x > 0 ? d.spacing.x : 76, cellH = d.spacing.y > 0 ? d.spacing.y : 101;
    const LONG left = d.primary.left + 4, top = d.primary.top + 4, right = d.primary.right - 4;
    const int rows = std::max<int>(1, int((d.primary.bottom - top) / cellH));
    std::set<std::pair<LONG, LONG>> used;  // occupied grid cells (screen coords, cell units)
    auto cellOf = [&](POINT p) { return std::make_pair((p.x - left) / cellW, (p.y - top) / cellH); };
    auto place = [&](const Item& it, POINT screen) {
        POINT pt{screen.x - d.origin.x, screen.y - d.origin.y};
        PCUITEMID_CHILD one = it.pidl;
        d.view->SelectAndPositionItems(1, &one, &pt, SVSI_POSITIONITEM);
        used.insert(cellOf(screen));
    };

    std::vector<const Item*> pending;
    for (const Item& it : items) {
        auto s = saved.find(it.name);
        if (s == saved.end()) {
            pending.push_back(&it);
            continue;
        }
        place(it, POINT{d.primary.left + s->second.x, d.primary.top + s->second.y});
        r.placed++;
    }
    // New icons: first free cell scanning columns from the left (programs) or from the right (folders).
    for (const Item* it : pending) {
        const bool folder = isFolderLike(it->pidl);
        for (int col = 0;; ++col) {
            const LONG x = folder ? right - (col + 1) * cellW : left + col * cellW;
            if (x < d.primary.left || x + cellW > d.primary.right + cellW) break;
            int row = 0;
            for (; row < rows; ++row)
                if (!used.count(cellOf(POINT{x, top + row * cellH}))) break;
            if (row < rows) {
                place(*it, POINT{x, top + row * cellH});
                r.added++;
                break;
            }
        }
    }
    r.ok = true;
    MSD_INFO("desktop: restored %d/%d icons (%d new placed by rule)", r.placed, r.total, r.added);
    freeItems(items);
    return r;
}

}  // namespace msdisp
