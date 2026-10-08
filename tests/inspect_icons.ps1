param(
    [Parameter(Mandatory)][string]$Executable,
    [int]$TestProcessId = 0,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\build\icons')
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class IconInspection {
    public delegate bool EnumProc(IntPtr window, IntPtr value);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr value);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wp, IntPtr lp, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll", EntryPoint="GetClassLongPtrW")] public static extern IntPtr GetClassLongPtr(IntPtr window, int index);
    [DllImport("user32.dll")] public static extern bool DestroyIcon(IntPtr icon);
    [DllImport("shell32.dll", CharSet=CharSet.Unicode)] public static extern uint ExtractIconEx(string path, int index, out IntPtr large, out IntPtr small, uint count);
    public static IntPtr FindMainWindow(int process) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((window, value) => {
            uint owner; GetWindowThreadProcessId(window, out owner);
            if (owner == process) {
                var text = new StringBuilder(256);
                GetClassName(window, text, 256);
                if (text.ToString() == "MicroSIP") result = window;
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }
}
'@
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
function Test-Icon([IntPtr]$Handle, [string]$Name) {
    if ($Handle -eq [IntPtr]::Zero) { throw "Missing $Name icon handle" }
    $icon = [System.Drawing.Icon]::FromHandle($Handle)
    $bitmap = $icon.ToBitmap()
    try {
        $visiblePixels = 0
        for ($x = 0; $x -lt $bitmap.Width; ++$x) {
            for ($y = 0; $y -lt $bitmap.Height; ++$y) {
                if ($bitmap.GetPixel($x,$y).A -gt 0) { ++$visiblePixels }
            }
        }
        if (!$visiblePixels) { throw "$Name icon is entirely transparent" }
        $bitmap.Save((Join-Path $OutputDirectory "$Name.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        "PASS: $Name $($bitmap.Width)x$($bitmap.Height), $visiblePixels visible pixels"
    } finally { $bitmap.Dispose() }
}
[IntPtr]$large = [IntPtr]::Zero
[IntPtr]$small = [IntPtr]::Zero
if ([IconInspection]::ExtractIconEx((Resolve-Path $Executable).Path, 0, [ref]$large, [ref]$small, 1) -eq 0) {
    throw 'Unable to extract executable application icon'
}
try {
    Test-Icon $large 'executable-large'
    Test-Icon $small 'executable-small'
} finally {
    if ($large -ne [IntPtr]::Zero) { [IconInspection]::DestroyIcon($large) | Out-Null }
    if ($small -ne [IntPtr]::Zero) { [IconInspection]::DestroyIcon($small) | Out-Null }
}
if ($TestProcessId) {
    $window = [IconInspection]::FindMainWindow($TestProcessId)
    if ($window -eq [IntPtr]::Zero) { throw 'Test application window not found' }
    foreach ($kind in @(@(1,'window-large'),@(0,'window-small'),@(2,'window-small2'))) {
        [IntPtr]$handle = [IntPtr]::Zero
        if ([IconInspection]::SendMessageTimeout($window, 0x7f, [IntPtr]$kind[0], [IntPtr]::Zero, 2, 2000, [ref]$handle) -eq [IntPtr]::Zero) {
            throw "WM_GETICON timed out for $($kind[1])"
        }
        Test-Icon $handle $kind[1]
    }
    Test-Icon ([IconInspection]::GetClassLongPtr($window,-14)) 'class-large'
    Test-Icon ([IconInspection]::GetClassLongPtr($window,-34)) 'class-small'
}
