param([Parameter(Mandatory)][int]$TestProcessId)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class DialerPreview {
    public delegate bool EnumProc(IntPtr handle, IntPtr arg);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr arg);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr arg);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr handle, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr handle, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr handle, int id);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr handle);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr handle, out Rect rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr handle, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr handle, int command);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr handle, IntPtr after, int x, int y, int width, int height, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    public static IntPtr Main(int process) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((h, p) => { uint pid; GetWindowThreadProcessId(h, out pid);
            var name = new StringBuilder(256); GetClassName(h, name, 256);
            if (pid == process && name.ToString() == "MicroSIP") result = h;
            return true;
        }, IntPtr.Zero);
        return result;
    }
    public static IntPtr Dialer(IntPtr main) {
        IntPtr result = IntPtr.Zero;
        EnumChildWindows(main, (h, p) => {
            if (GetDlgItem(h, 1078) != IntPtr.Zero && GetDlgItem(h, 1135) != IntPtr.Zero) result = h;
            return true;
        }, IntPtr.Zero);
        return result;
    }
}
'@
function Get-Rect([IntPtr]$Window) {
    $rect = New-Object DialerPreview+Rect
    if (-not [DialerPreview]::GetWindowRect($Window, [ref]$rect)) { throw 'Window rectangle unavailable' }
    return $rect
}
function Save-Preview([IntPtr]$Window, [string]$Name) {
    $rect = Get-Rect $Window
    $bitmap = New-Object System.Drawing.Bitmap ($rect.Right-$rect.Left),($rect.Bottom-$rect.Top)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $hdc = $graphics.GetHdc()
    try { [DialerPreview]::PrintWindow($Window, $hdc, 2) | Out-Null }
    finally { $graphics.ReleaseHdc($hdc); $graphics.Dispose() }
    $bitmap.Save((Join-Path $PSScriptRoot "..\build\$Name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $bitmap.Dispose()
}
$main = [DialerPreview]::Main($TestProcessId)
if ($main -eq [IntPtr]::Zero) { throw 'Isolated test MicroSIP window not found' }
[DialerPreview]::ShowWindow($main, 4) | Out-Null
$dialer = [DialerPreview]::Dialer($main)
$initial = Get-Rect $main
$number = [DialerPreview]::GetDlgItem($dialer, 1078)
$call = [DialerPreview]::GetDlgItem($dialer, 1022)
$message = [DialerPreview]::GetDlgItem($dialer, 1076)
if ([DialerPreview]::IsWindowVisible($message)) { throw 'Message button is visible in the custom voice build' }
$numberRect = Get-Rect $number
$top = $numberRect.Top - (Get-Rect $dialer).Top
try {
    foreach ($mode in @('idle', 'resized')) {
        if ($mode -eq 'resized') {
            [DialerPreview]::SetWindowPos($main, [IntPtr]::Zero, 0, 0,
                $initial.Right-$initial.Left+180, $initial.Bottom-$initial.Top+180, 0x16) | Out-Null
            Start-Sleep -Milliseconds 200
        }
        $numberRect = Get-Rect $number
        $callRect = Get-Rect $call
        if ([Math]::Abs(($numberRect.Top-(Get-Rect $dialer).Top)-$top) -gt 1) { throw 'Number field moved down when resizing' }
        if ([Math]::Abs($numberRect.Left-$callRect.Left) -gt 1 -or [Math]::Abs($numberRect.Right-$callRect.Right) -gt 1) { throw 'Idle Call button does not span the dialer width' }
        Save-Preview $main "dialer-$mode"
    }
}
finally {
    [DialerPreview]::SetWindowPos($main, [IntPtr]::Zero, 0, 0,
        $initial.Right-$initial.Left, $initial.Bottom-$initial.Top, 0x16) | Out-Null
}
Write-Output 'PASS: Message hidden, full-width Call button, number field stays at top during resize; PNGs saved'
