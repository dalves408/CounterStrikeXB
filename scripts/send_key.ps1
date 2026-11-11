<#
 Diagnostic harness, not for normal play. Posts WM_KEYDOWN/WM_KEYUP to the
 game's own framebuffer window. Those messages go through the toolkit's
 keyboard map -> pad 0 -> USB XID report -> OHCI -> XAPI, the same path a real
 keyboard takes. No guest memory is touched.
   .\scripts\send_key.ps1 -ProcessId 1234 -Key Return
#>
param([Parameter(Mandatory)][int]$ProcessId, [Parameter(Mandatory)][string]$Key, [int]$HoldMs = 150)
Add-Type -AssemblyName System.Windows.Forms
if (-not ('CS2Win.W' -as [type])) {
    Add-Type -Namespace CS2Win -Name W -MemberDefinition '
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Ansi)] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string t);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern uint MapVirtualKey(uint c, uint t);
'
}
$vk = [int]([System.Windows.Forms.Keys]$Key)
$h = [IntPtr]::Zero; $found = [IntPtr]::Zero
while (($h = [CS2Win.W]::FindWindowEx([IntPtr]::Zero, $h, 'XboxRecompFramebuffer', $null)) -ne [IntPtr]::Zero) {
    $wp = [uint32]0; [CS2Win.W]::GetWindowThreadProcessId($h, [ref]$wp) | Out-Null
    if ($wp -eq $ProcessId) { $found = $h; break }
}
if ($found -eq [IntPtr]::Zero) { throw "no XboxRecompFramebuffer window for PID $ProcessId" }
$scan = [int64][CS2Win.W]::MapVirtualKey([uint32]$vk, 0)
$down = [IntPtr]([int64]1 -bor ($scan -shl 16))
$up = [IntPtr]([int64]1 -bor ($scan -shl 16) -bor ([int64]1 -shl 30) -bor ([int64]1 -shl 31))
[CS2Win.W]::PostMessage($found, 0x0100, [IntPtr]$vk, $down) | Out-Null
Start-Sleep -Milliseconds $HoldMs
[CS2Win.W]::PostMessage($found, 0x0101, [IntPtr]$vk, $up) | Out-Null
Write-Host "sent $Key (vk 0x$('{0:X2}' -f $vk)) to PID $ProcessId"
