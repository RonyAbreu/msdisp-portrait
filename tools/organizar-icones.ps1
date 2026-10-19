# Arranges desktop icons on the primary monitor: programs (shortcuts, apps, files, shell items) in columns from the
# left edge, folders (and shortcuts to folders) in columns from the right edge. Turns auto-arrange off, keeps grid.
param([switch]$DryRun)
$src = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

[StructLayout(LayoutKind.Sequential)] public struct PT { public int X, Y; }

[ComImport, Guid("85CB6900-4D95-11CF-960C-0080C7F4EE85"), InterfaceType(ComInterfaceType.InterfaceIsDual)]
public interface IShellWindowsX {
    int Count { get; }
    [return: MarshalAs(UnmanagedType.IDispatch)] object Item([In, Optional] object index);
    [return: MarshalAs(UnmanagedType.IUnknown)] object _NewEnum();
    void Register([MarshalAs(UnmanagedType.IDispatch)] object pid, int hwnd, int swClass, out int plCookie);
    void RegisterPending(int lThreadId, ref object pvarloc, ref object pvarlocRoot, int swClass, out int plCookie);
    void Revoke(int lCookie);
    void OnNavigate(int lCookie, ref object pvarLoc);
    void OnActivated(int lCookie, bool fActive);
    [return: MarshalAs(UnmanagedType.IDispatch)] object FindWindowSW(ref object pvarLoc, ref object pvarLocRoot, int swClass, out int phwnd, int swfwOptions);
}
[ComImport, Guid("6D5140C1-7436-11CE-8034-00AA006009FA"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IServiceProviderX { [return: MarshalAs(UnmanagedType.IUnknown)] object QueryService(ref Guid s, ref Guid r); }
[ComImport, Guid("000214E2-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IShellBrowserX {
    void GetWindow(out IntPtr h); void ContextSensitiveHelp(bool f); void InsertMenusSB(IntPtr a, IntPtr b);
    void SetMenuSB(IntPtr a, IntPtr b, IntPtr c); void RemoveMenusSB(IntPtr a); void SetStatusTextSB(IntPtr a);
    void EnableModelessSB(bool f); void TranslateAcceleratorSB(IntPtr a, short b); void BrowseObject(IntPtr a, uint b);
    void GetViewStateStream(uint a, out IntPtr b); void GetControlWindow(uint a, out IntPtr b);
    void SendControlMsg(uint a, uint b, IntPtr c, IntPtr d, out IntPtr e);
    [return: MarshalAs(UnmanagedType.IUnknown)] object QueryActiveShellView();
}
[ComImport, Guid("1af3a467-214f-4298-908e-06b03e0b39f9"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
public interface IFolderView2X {
    void GetCurrentViewMode(out uint m); void SetCurrentViewMode(uint m); void GetFolder(ref Guid r, out IntPtr p);
    void Item(int i, out IntPtr pidl); void ItemCount(uint f, out int c); void Items(uint f, ref Guid r, out IntPtr p);
    void GetSelectionMarkedItem(out int i); void GetFocusedItem(out int i);
    void GetItemPosition(IntPtr pidl, out PT pt); void GetSpacing(out PT pt); void GetDefaultSpacing(out PT pt);
    [PreserveSig] int GetAutoArrange(); void SelectItem(int i, uint f);
    void SelectAndPositionItems(uint cidl, [MarshalAs(UnmanagedType.LPArray)] IntPtr[] apidl, [MarshalAs(UnmanagedType.LPArray)] PT[] apt, uint flags);
    void SetGroupBy(IntPtr k, bool a); void GetGroupBy(out IntPtr k, out bool a);
    void SetViewProperty(IntPtr p, IntPtr k, IntPtr v); void GetViewProperty(IntPtr p, IntPtr k, IntPtr v);
    void SetTileViewProperties(IntPtr p, IntPtr s); void SetExtendedTileViewProperties(IntPtr p, IntPtr s);
    void SetText(int t, [MarshalAs(UnmanagedType.LPWStr)] string s);
    void SetCurrentFolderFlags(uint mask, uint flags); void GetCurrentFolderFlags(out uint flags);
}

public static class DeskLayout {
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)] static extern int SHGetNameFromIDList(IntPtr pidl, uint sigdn, out IntPtr name);
    static IFolderView2X fv;
    static IntPtr[] pidls;

    static string Name(IntPtr pidl, uint sigdn) {
        IntPtr p;
        if (SHGetNameFromIDList(pidl, sigdn, out p) != 0) return "";
        string s = Marshal.PtrToStringUni(p); Marshal.FreeCoTaskMem(p); return s;
    }

    public static string[] List() {
        var sw = (IShellWindowsX)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("9BA05972-F6A8-11CF-A442-00A0C90A8F39")));
        object loc = 0, root = Type.Missing; int hwnd;
        var sp = (IServiceProviderX)sw.FindWindowSW(ref loc, ref root, 8, out hwnd, 1);
        Guid sid = new Guid("4C96BE40-915C-11CF-99D3-00AA004AE837"), iid = typeof(IShellBrowserX).GUID;
        fv = (IFolderView2X)((IShellBrowserX)sp.QueryService(ref sid, ref iid)).QueryActiveShellView();
        int n; fv.ItemCount(2, out n);
        pidls = new IntPtr[n];
        var res = new string[n];
        for (int i = 0; i < n; i++) {
            fv.Item(i, out pidls[i]);
            PT pt; fv.GetItemPosition(pidls[i], out pt);
            res[i] = i + "|" + Name(pidls[i], 0) + "|" + Name(pidls[i], 0x80058000) + "|" + pt.X + "|" + pt.Y;
        }
        return res;
    }

    public static string Spacing() { PT s; fv.GetSpacing(out s); return s.X + "|" + s.Y; }

    public static void Place(int[] idx, int[] xs, int[] ys) {
        fv.SetCurrentFolderFlags(0x1 | 0x4, 0x4);  // auto-arrange off, snap to grid on
        for (int k = 0; k < idx.Length; k++) {
            var pt = new PT { X = xs[k], Y = ys[k] };
            fv.SelectAndPositionItems(1, new[] { pidls[idx[k]] }, new[] { pt }, 0x80 /* SVSI_POSITIONITEM */);
        }

    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp
Add-Type -AssemblyName System.Windows.Forms

$items = [DeskLayout]::List() | ForEach-Object { $f = $_ -split '\|'; [pscustomobject]@{ I = [int]$f[0]; Name = $f[1]; Path = $f[2]; X = [int]$f[3]; Y = [int]$f[4] } }
$ws = New-Object -ComObject WScript.Shell
foreach ($it in $items) {
    $isFolder = $false
    if ($it.Path -and (Test-Path -LiteralPath $it.Path -PathType Container)) { $isFolder = $true }
    elseif ($it.Path -like '*.lnk') { try { $t = $ws.CreateShortcut($it.Path).TargetPath; if ($t -and (Test-Path -LiteralPath $t -PathType Container)) { $isFolder = $true } } catch {} }
    $it | Add-Member Kind $(if ($isFolder) { 'pasta' } else { 'programa' })
}

# Desktop item coordinates are relative to the primary monitor's top-left (measured: auto-arranged icons start at 0).
$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
$wa = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$sp = [DeskLayout]::Spacing() -split '\|'; $sx = [int]$sp[0]; $sy = [int]$sp[1]
$left = $wa.Left + 4; $top = $wa.Top + 4
$right = $wa.Right - 4; $rows = [Math]::Floor(($wa.Height - 8) / $sy)
"virtual screen left=$($vs.Left) work area=$wa spacing=${sx}x${sy} rows=$rows"

$progs = $items | Where-Object Kind -eq 'programa' | Sort-Object X, Y
$dirs = $items | Where-Object Kind -eq 'pasta' | Sort-Object X, Y
$idx = @(); $xs = @(); $ys = @()
$k = 0; foreach ($it in $progs) { $idx += $it.I; $xs += [int]($left + [Math]::Floor($k / $rows) * $sx); $ys += [int]($top + ($k % $rows) * $sy); $k++ }
$k = 0; foreach ($it in $dirs) { $idx += $it.I; $xs += [int]($right - ([Math]::Floor($k / $rows) + 1) * $sx); $ys += [int]($top + ($k % $rows) * $sy); $k++ }
"programas: $($progs.Count) em $([Math]::Ceiling($progs.Count / $rows)) colunas; pastas: $($dirs.Count) em $([Math]::Ceiling($dirs.Count / $rows)) colunas"
"pastas: " + (($dirs | ForEach-Object Name) -join ', ')
if (-not $DryRun) { [DeskLayout]::Place([int[]]$idx, [int[]]$xs, [int[]]$ys); "posicionados: $($idx.Count)" }
