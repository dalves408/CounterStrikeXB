<#
 Bounded diagnostic run with framebuffer snapshots and an optional key schedule.
   .\scripts\probe.ps1 -Label boot1 -Seconds 120
   .\scripts\probe.ps1 -Label nav1 -Seconds 150 -Keys '60:Return','70:Down','72:Z'
 Snapshots: runs\<label>\snap_<sec>.png copied from the guest framebuffer dump
 (what the CRTC scans out, i.e. what a person sees). Keys go through
 send_key.ps1 (real keyboard path). Stops only the process it started.
#>
param(
    [Parameter(Mandatory)][string]$Label,
    [int]$Seconds = 120,
    [int]$SnapEvery = 5,
    [string[]]$Keys = @(),          # '<sec>:<KeyName>' e.g. '45:Return' (';'-separated ok)
    [string[]]$ExtraEnv = @()
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path $PSScriptRoot -Parent
$p = & (Join-Path $PSScriptRoot 'run.ps1') -Label $Label -FbDumpEvery 30 -ExtraEnv ($ExtraEnv -join ';') -PassThru
$dir = Join-Path $root "runs\$Label"
$fb = Join-Path $dir 'fb.bmp'
$sched = foreach ($k in (($Keys -join ';') -split '[;,]' | Where-Object { $_ })) { $t, $n = $k -split ':', 2; [pscustomobject]@{ T = [double]$t; Key = $n; Done = $false } }
$sw = [Diagnostics.Stopwatch]::StartNew(); $nextSnap = $SnapEvery; $lastHash = ''
"t,event" | Set-Content (Join-Path $dir 'probe.csv')
$pidFile = Join-Path $dir 'pid.txt'
function Get-Live { @(Get-Content $pidFile | ForEach-Object { Get-Process -Id ([int]$_) -ErrorAction SilentlyContinue } | Where-Object { $_.ProcessName -eq 'cs2_xbox_recomp' }) }
while ($sw.Elapsed.TotalSeconds -lt $Seconds -and (Get-Live).Count -gt 0) {
    $cur = (Get-Live)[-1]
    Start-Sleep -Milliseconds 250
    $t = $sw.Elapsed.TotalSeconds
    foreach ($s in $sched) {
        if (-not $s.Done -and $t -ge $s.T) {
            $s.Done = $true
            try { & (Join-Path $PSScriptRoot 'send_key.ps1') -ProcessId $cur.Id -Key $s.Key | Out-Null; "$([int]$t),key $($s.Key)" | Add-Content (Join-Path $dir 'probe.csv') }
            catch { "$([int]$t),key $($s.Key) FAILED $_" | Add-Content (Join-Path $dir 'probe.csv') }
        }
    }
    if ($t -ge $nextSnap) {
        $nextSnap += $SnapEvery
        if (Test-Path $fb) {
            try {
                $bytes = [IO.File]::ReadAllBytes($fb)
                $h = [BitConverter]::ToString((New-Object Security.Cryptography.SHA1Managed).ComputeHash($bytes))
                if ($h -ne $lastHash) {
                    $lastHash = $h
                    $ms = New-Object IO.MemoryStream(, $bytes); $img = [Drawing.Image]::FromStream($ms)
                    $out = Join-Path $dir ('snap_{0:000}.png' -f [int]$t); $img.Save($out, [Drawing.Imaging.ImageFormat]::Png); $img.Dispose(); $ms.Dispose()
                    "$([int]$t),snap $(Split-Path $out -Leaf)" | Add-Content (Join-Path $dir 'probe.csv')
                }
            } catch { }   # dump being rewritten; take the next one
        }
    }
}
$left = Get-Live
if ($left) { foreach ($q in $left) { Stop-Process -Id $q.Id -Force; "stopped PID $($q.Id) at $Seconds s" } }
else { "chain exited at $([int]$sw.Elapsed.TotalSeconds) s" }
"pids: $((Get-Content $pidFile) -join ', ')"
$klog = Join-Path $root 'xbox_kernel.log'
if (Test-Path $klog) { Copy-Item $klog (Join-Path $dir 'xbox_kernel.log') -Force }
Get-ChildItem $dir | Select-Object Name, Length | Format-Table -AutoSize | Out-String
