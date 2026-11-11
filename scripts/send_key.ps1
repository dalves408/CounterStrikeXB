<#
 Diagnostic harness, not for normal play. Posts WM_KEYDOWN/WM_KEYUP to the
 game's own framebuffer window. Those messages go through the toolkit's
 keyboard map -> pad 0 -> USB XID report -> OHCI -> XAPI, the same path a real
 keyboard takes. No guest memory is touched.
   .\scripts\send_key.ps1 -ProcessId 1234 -Key Return
 The window lookup is done in C# (EnumWindows): a PowerShell scriptblock used
 as the callback could not see this script's parameters, so it never matched.
#>
param([Parameter(Mandatory)][int]$ProcessId, [Parameter(Mandatory)][string]$Key, [int]$HoldMs = 150)
Add-Type -AssemblyName System.Windows.Forms
if (-not ('CS2Key.Win' -as [type])) {
    Add-Type -TypeDefinition @"
using System; using System.Text; using System.Runtime.InteropServices;
namespace CS2Key {
public static class Win {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Ansi)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint c, uint t);
    public static IntPtr Find(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr h, IntPtr l) {
            uint wp; GetWindowThreadProcessId(h, out wp);
            if (wp != pid) return true;
            StringBuilder sb = new StringBuilder(64); GetClassName(h, sb, 64);
            if (sb.ToString() == "XboxRecompFramebuffer") { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}}
"@
}
$vk = [int]([System.Windows.Forms.Keys]$Key)
$found = [CS2Key.Win]::Find([uint32]$ProcessId)
if ($found -eq [IntPtr]::Zero) { throw "no XboxRecompFramebuffer window for PID $ProcessId" }
$scan = [int64][CS2Key.Win]::MapVirtualKey([uint32]$vk, 0)
$down = [IntPtr]([int64]1 -bor ($scan -shl 16))
$up = [IntPtr]([int64]1 -bor ($scan -shl 16) -bor ([int64]1 -shl 30) -bor ([int64]1 -shl 31))
[CS2Key.Win]::PostMessage($found, 0x0100, [IntPtr]$vk, $down) | Out-Null
Start-Sleep -Milliseconds $HoldMs
[CS2Key.Win]::PostMessage($found, 0x0101, [IntPtr]$vk, $up) | Out-Null
Write-Host "sent $Key (vk 0x$('{0:X2}' -f $vk)) to PID $ProcessId"