# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) Dave Wheitner and MEGAN XG contributors

# ShotKit.ps1 — screenshot harness for the MEGAN XG standalone.
#
# Dot-source this, then use Shot-* functions. Every synthetic click is guarded on
# WindowFromPoint belonging to the TARGET PROCESS: SetForegroundWindow can fail
# silently (focus-stealing prevention), and an unguarded click then lands in
# whatever app is actually in front.

Add-Type -AssemblyName System.Drawing

Add-Type @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Collections.Generic;

public class Shot {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int t,bool r);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint x,uint y,uint d,UIntPtr e);
    [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
    [DllImport("user32.dll")] public static extern IntPtr GetDesktopWindow();
    [DllImport("user32.dll")] public static extern short VkKeyScan(char c);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint f, UIntPtr e);

    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left,Top,Right,Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }

    const uint LDOWN=0x0002, LUP=0x0004, RDOWN=0x0008, RUP=0x0010, WHEEL=0x0800;

    // All visible top-level windows of a process, largest first.
    public static List<IntPtr> Windows(int pid) {
        var list = new List<IntPtr>();
        EnumWindows((h,p) => {
            int wp; GetWindowThreadProcessId(h, out wp);
            if (wp == pid && IsWindowVisible(h)) {
                RECT r; GetWindowRect(h, out r);
                if (r.Right-r.Left > 50 && r.Bottom-r.Top > 50) list.Add(h);
            }
            return true;
        }, IntPtr.Zero);
        list.Sort((a,b) => Area(b).CompareTo(Area(a)));
        return list;
    }
    static long Area(IntPtr h){ RECT r; GetWindowRect(h,out r); return (long)(r.Right-r.Left)*(r.Bottom-r.Top); }

    public static string Title(IntPtr h) {
        var sb = new System.Text.StringBuilder(GetWindowTextLength(h)+1);
        GetWindowText(h, sb, sb.Capacity); return sb.ToString();
    }

    // True only if the point really belongs to a window of `pid`.
    public static bool PointOwnedBy(int x, int y, int pid) {
        POINT p; p.X=x; p.Y=y;
        IntPtr h = WindowFromPoint(p);
        if (h == IntPtr.Zero) return false;
        int wp; GetWindowThreadProcessId(h, out wp);
        return wp == pid;
    }

    public static bool Click(int x, int y, int pid, bool right) {
        SetCursorPos(x,y);
        System.Threading.Thread.Sleep(60);
        if (!PointOwnedBy(x,y,pid)) return false;      // refuse to click elsewhere
        mouse_event(right?RDOWN:LDOWN,0,0,0,UIntPtr.Zero);
        System.Threading.Thread.Sleep(40);
        mouse_event(right?RUP:LUP,0,0,0,UIntPtr.Zero);
        System.Threading.Thread.Sleep(120);
        return true;
    }

    public static bool Drag(int x1,int y1,int x2,int y2,int pid,bool right) {
        SetCursorPos(x1,y1);
        System.Threading.Thread.Sleep(60);
        if (!PointOwnedBy(x1,y1,pid)) return false;
        mouse_event(right?RDOWN:LDOWN,0,0,0,UIntPtr.Zero);
        int steps = 18;
        for (int i=1;i<=steps;i++) {
            SetCursorPos(x1+(x2-x1)*i/steps, y1+(y2-y1)*i/steps);
            System.Threading.Thread.Sleep(15);
        }
        mouse_event(right?RUP:RUP,0,0,0,UIntPtr.Zero);
        if(!right) mouse_event(LUP,0,0,0,UIntPtr.Zero);
        System.Threading.Thread.Sleep(120);
        return true;
    }

    public static void Move(int x,int y){ SetCursorPos(x,y); System.Threading.Thread.Sleep(80); }

    // Capture a window. PrintWindow(PW_RENDERFULLCONTENT) first — it works with
    // the window occluded; falls back to grabbing the screen region.
    public static void Capture(IntPtr h, string path, int cropL,int cropT,int cropR,int cropB) {
        RECT r; GetWindowRect(h, out r);
        int w = r.Right-r.Left, t = r.Bottom-r.Top;
        using (var bmp = new Bitmap(w,t,PixelFormat.Format32bppArgb)) {
            using (var g = Graphics.FromImage(bmp)) {
                IntPtr dc = g.GetHdc();
                bool ok = PrintWindow(h, dc, 2);
                g.ReleaseHdc(dc);
                if (!ok) {
                    using (var g2 = Graphics.FromImage(bmp))
                        g2.CopyFromScreen(r.Left,r.Top,0,0,new Size(w,t));
                }
            }
            int x=cropL, y=cropT, cw=w-cropL-cropR, ch=t-cropT-cropB;
            if (cw<=0||ch<=0){ bmp.Save(path, ImageFormat.Png); return; }
            using (var cut = bmp.Clone(new Rectangle(x,y,cw,ch), PixelFormat.Format32bppArgb))
                cut.Save(path, ImageFormat.Png);
        }
    }

    // Whole desktop. Needed for POPUP MENUS: a JUCE PopupMenu is its own
    // top-level window, so PrintWindow on the main window never contains it.
    public static void CaptureScreen(string path, int x,int y,int w,int h) {
        using (var bmp = new Bitmap(w,h,PixelFormat.Format32bppArgb)) {
            using (var g = Graphics.FromImage(bmp)) g.CopyFromScreen(x,y,0,0,new Size(w,h));
            bmp.Save(path, ImageFormat.Png);
        }
    }

    // Is the captured image blank (PrintWindow sometimes yields all-black)?
    public static string Describe(string path) {
        using (var b = new Bitmap(path)) {
            long tot=0; int n=0; var seen=new HashSet<int>();
            for (int y=0;y<b.Height;y+=7) for (int x=0;x<b.Width;x+=7) {
                var c=b.GetPixel(x,y); tot+=c.R+c.G+c.B; n++;
                if (seen.Count<400) seen.Add((c.R>>3<<10)|(c.G>>3<<5)|(c.B>>3));
            }
            return b.Width+"x"+b.Height+" mean="+(tot/(3.0*n)).ToString("F1")+" colours="+seen.Count;
        }
    }
}
'@ -ReferencedAssemblies System.Drawing, System.Collections

$script:ShotProc = $null
$script:ShotHwnd = [IntPtr]::Zero

# Resolved here, at the script's top level, where $PSScriptRoot is this file's
# folder however the kit was dot-sourced -- so the kit works from any clone.
$script:ShotImagesDir = Join-Path $PSScriptRoot "..\images"
$script:ShotExe = Join-Path $PSScriptRoot "..\..\..\Builds\VisualStudio2026\x64\Release\Standalone Plugin\MEGAN XG.exe"

function Shot-Launch {
    param([int]$Width = 1600, [int]$Height = 1000, [int]$WaitSec = 22)
    $exe = $script:ShotExe
    $script:ShotProc = Start-Process $exe -PassThru
    $deadline = (Get-Date).AddSeconds($WaitSec)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 700
        $w = [Shot]::Windows($script:ShotProc.Id)
        if ($w.Count -gt 0) { $script:ShotHwnd = $w[0]; break }
    }
    if ($script:ShotHwnd -eq [IntPtr]::Zero) { throw "no window appeared" }
    Start-Sleep -Seconds 6     # let MAME boot and the LCD come up
    Shot-Size $Width $Height
    "pid=$($script:ShotProc.Id) hwnd=$($script:ShotHwnd) title='$([Shot]::Title($script:ShotHwnd))'"
}

function Shot-Size { param([int]$W,[int]$H)
    [void][Shot]::MoveWindow($script:ShotHwnd, 60, 40, $W, $H, $true)
    Start-Sleep -Milliseconds 600
    Shot-Front
}

function Shot-Front {
    for ($i=0; $i -lt 6; $i++) {
        [void][Shot]::SetForegroundWindow($script:ShotHwnd)
        Start-Sleep -Milliseconds 250
        if ([Shot]::GetForegroundWindow() -eq $script:ShotHwnd) { return $true }
    }
    Write-Warning "could not bring the plugin window to the front"
    return $false
}

# Client-area coordinates -> screen
function Shot-Pt { param([int]$X,[int]$Y,[IntPtr]$Hwnd=[IntPtr]::Zero)
    if ($Hwnd -eq [IntPtr]::Zero) { $Hwnd = $script:ShotHwnd }
    $p = New-Object Shot+POINT; $p.X=$X; $p.Y=$Y
    [void][Shot]::ClientToScreen($Hwnd, [ref]$p)
    return $p
}

function Shot-Click { param([int]$X,[int]$Y,[switch]$Right,[IntPtr]$Hwnd=[IntPtr]::Zero)
    $p = Shot-Pt $X $Y $Hwnd
    $ok = [Shot]::Click($p.X,$p.Y,$script:ShotProc.Id,[bool]$Right)
    if (-not $ok) { Write-Warning "click at client($X,$Y) REFUSED - point not owned by the plugin" }
    return $ok
}

function Shot-Drag { param([int]$X1,[int]$Y1,[int]$X2,[int]$Y2,[switch]$Right)
    $a = Shot-Pt $X1 $Y1; $b = Shot-Pt $X2 $Y2
    $ok = [Shot]::Drag($a.X,$a.Y,$b.X,$b.Y,$script:ShotProc.Id,[bool]$Right)
    if (-not $ok) { Write-Warning "drag from client($X1,$Y1) REFUSED" }
    return $ok
}

function Shot-Hover { param([int]$X,[int]$Y)
    $p = Shot-Pt $X $Y; [Shot]::Move($p.X,$p.Y)
}

# Save a capture into the manual's images folder.
# Park the cursor on blank page background and wait out any tooltip. A hover
# tip is a real window that lands IN the capture, so every shot parks first.
function Shot-Park {
    $p = Shot-Pt 40 500
    [Shot]::Move($p.X,$p.Y)
    Start-Sleep -Milliseconds 1100
}

function Shot-Save {
    param([string]$Name,[int]$L=0,[int]$T=0,[int]$R=0,[int]$B=0,[IntPtr]$Hwnd=[IntPtr]::Zero,[switch]$NoPark)
    if ($Hwnd -eq [IntPtr]::Zero) { $Hwnd = $script:ShotHwnd }
    $dir = $script:ShotImagesDir
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $path = Join-Path $dir $Name
    if (-not $NoPark) { Shot-Park }
    Start-Sleep -Milliseconds 300
    [Shot]::Capture($Hwnd, $path, $L,$T,$R,$B)
    "$Name  $([Shot]::Describe($path))"
}

# Capture a rectangle of the SCREEN, in client coords of the main window.
function Shot-Screen {
    param([string]$Name,[int]$X=0,[int]$Y=0,[int]$W=1600,[int]$H=1000)
    $dir = $script:ShotImagesDir
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $path = Join-Path $dir $Name
    $p = Shot-Pt $X $Y
    [Shot]::CaptureScreen($path, $p.X, $p.Y, $W, $H)
    "$Name  $([Shot]::Describe($path))"
}

function Shot-Windows {
    [Shot]::Windows($script:ShotProc.Id) | ForEach-Object {
        $r = New-Object Shot+RECT; [void][Shot]::GetWindowRect($_, [ref]$r)
        "$_  $($r.Right-$r.Left)x$($r.Bottom-$r.Top)  '$([Shot]::Title($_))'"
    }
}

function Shot-Quit {
    if ($script:ShotProc -and -not $script:ShotProc.HasExited) {
        $script:ShotProc.CloseMainWindow() | Out-Null
        Start-Sleep -Seconds 2
        if (-not $script:ShotProc.HasExited) { $script:ShotProc.Kill() }
    }
}
