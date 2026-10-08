<#
Checks the detached conversation window's actual Unicode caption.
Use only an isolated portable test copy with a blank localhost account,
singleMode=0 and callAudioMode=0; establish a synthetic localhost SIP call.
Pass that test process PID and its expected display name/number. This script
does not launch, close, or change any softphone instance or user account.
Example after establishing a synthetic title-probe call on local port 55000:
  .\tests\inspect_title.ps1 -TestProcessId 1234 `
      -ExpectedName 'title-probe@127.0.0.1:55000' -ExpectedNumber 'title-probe'
#>
param(
    [Parameter(Mandatory)][int]$TestProcessId,
    [Parameter(Mandatory)][string]$ExpectedName,
    [Parameter(Mandatory)][string]$ExpectedNumber
)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class ConversationTitle {
    public delegate bool EnumProc(IntPtr handle, IntPtr arg);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr arg);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr handle, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr handle, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr handle, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr handle, int id);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr handle);
    public static IntPtr Find(int process) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((h, p) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == process) {
                var name = new StringBuilder(256);
                GetClassName(h, name, 256);
                if (name.ToString() != "#32770") return true;
                GetClassName(GetDlgItem(h, 1000), name, 256);
                if (name.ToString() == "SysTabControl32") result = h;
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }
    public static string Read(IntPtr window) {
        var caption = new StringBuilder(2048);
        GetWindowText(window, caption, caption.Capacity);
        return caption.ToString();
    }
}
'@
$window = [ConversationTitle]::Find($TestProcessId)
if ($window -eq [IntPtr]::Zero) { throw 'Conversation window not found in the specified test process' }
if (-not [ConversationTitle]::IsWindowVisible($window)) { throw 'Expected the visible detached window: use singleMode=0 in the isolated test profile' }
$expected = if ($ExpectedName -eq $ExpectedNumber) { $ExpectedNumber } else { "$ExpectedName $([char]0x2013) $ExpectedNumber" }
$actual = [ConversationTitle]::Read($window)
$points = ($actual.ToCharArray() | Where-Object { [int]$_ -gt 127 } | ForEach-Object { 'U+{0:X4}' -f [int]$_ }) -join ', '
Write-Output "Actual caption non-ASCII codepoints: $points"
if ($actual -cne $expected) { throw "Caption mismatch. Expected '$expected'; observed '$actual'" }
Write-Output 'PASS: detached multi-call window caption contains the intended Unicode separator and exact name/number'
