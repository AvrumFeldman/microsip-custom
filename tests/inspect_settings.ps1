param([Parameter(Mandatory)][int]$TestProcessId)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public class TestWindow {
    public delegate bool EnumProc(IntPtr handle, IntPtr arg);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr arg);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr handle, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr handle, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr handle, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr handle, uint message, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr handle, int id);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr handle);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr handle, out Rect rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr handle, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    public static IntPtr Find(int process, string match, bool byClass) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((h, p) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == process) {
                var text = new StringBuilder(256);
                if (byClass) GetClassName(h, text, 256); else GetWindowText(h, text, 256);
                if (text.ToString() == match) result = h;
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }
}
'@
$main = [TestWindow]::Find($TestProcessId, 'MicroSIP', $true)
if ($main -eq [IntPtr]::Zero) { throw 'Test MicroSIP window not found' }
[TestWindow]::SendMessage($main, 0x111, [IntPtr]32789, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
$settings = [TestWindow]::Find($TestProcessId, 'Settings', $false)
if ($settings -eq [IntPtr]::Zero) { throw 'Settings window not found' }
$mode = [TestWindow]::GetDlgItem($settings, 1300)
$apps = [TestWindow]::GetDlgItem($settings, 1301)
$browse = [TestWindow]::GetDlgItem($settings, 1302)
if ([TestWindow]::SendMessage($mode, 0x146, [IntPtr]::Zero, [IntPtr]::Zero).ToInt32() -ne 3) { throw 'Expected three audio options' }
foreach ($selection in @(0,1,2)) {
    [TestWindow]::SendMessage($mode, 0x14e, [IntPtr]$selection, [IntPtr]::Zero) | Out-Null
    [TestWindow]::SendMessage($settings, 0x111, [IntPtr](1300 -bor (1 -shl 16)), $mode) | Out-Null
    if ([TestWindow]::IsWindowEnabled($apps) -ne ($selection -eq 2)) { throw 'Selected app field enablement failed' }
    if ([TestWindow]::IsWindowEnabled($browse) -ne ($selection -eq 2)) { throw 'Browse button enablement failed' }
}
$rect = New-Object TestWindow+Rect
[TestWindow]::GetWindowRect($settings, [ref]$rect) | Out-Null
foreach ($control in @($mode,$apps,$browse,[TestWindow]::GetDlgItem($settings,1))) {
    $child = New-Object TestWindow+Rect
    [TestWindow]::GetWindowRect($control, [ref]$child) | Out-Null
    if ($child.Left -lt $rect.Left -or $child.Right -gt $rect.Right -or $child.Bottom -gt $rect.Bottom) { throw 'Settings control extends outside the window' }
}
$bitmap = New-Object System.Drawing.Bitmap ($rect.Right-$rect.Left),($rect.Bottom-$rect.Top)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$hdc = $graphics.GetHdc()
try { [TestWindow]::PrintWindow($settings, $hdc, 2) | Out-Null } finally { $graphics.ReleaseHdc($hdc); $graphics.Dispose() }
$imagePath=Join-Path $PSScriptRoot '..\build\settings.png'
$bitmap.Save($imagePath, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
[TestWindow]::SendMessage($settings, 0x111, [IntPtr]2, [IntPtr]::Zero) | Out-Null
Write-Output 'PASS: three settings modes, field enablement, controls fit; screenshot saved'
