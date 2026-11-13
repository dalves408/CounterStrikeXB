<#
 Straight-into-gameplay probe: starts the game with a saved launch page (the
 hand-over XLaunchNewImage gives the relaunched game), so the menus are skipped,
 then keeps pressing A/B to clear the tutorial prompts and buy menu.
   .\scripts\game_probe.ps1 -Label g1 [-Seconds 240] [-Page runs\launch_page_airstrip.bin]
       [-KeysFrom 60] [-KeyEvery 4] [-DumpAt 150,180] [-ExtraEnv 'A=1;B=2']
 Saving a page: run the menus with -ExtraEnv CS_LAUNCH_SAVE=<file> (Single Player ->
 Beginner -> Airstrip). Writes frames.csv (and RECOMP_CS_DUMP render targets at
 -DumpAt seconds into the run, 2 s each) under runs\<label>, then analyses it.
#>
param(
    [Parameter(Mandatory)][string]$Label,
    [int]$Seconds = 240,
    [string]$Page = 'runs\launch_page_airstrip.bin',
    [int]$KeysFrom = 60,
    [double]$KeyEvery = 4,
    [string]$Keys = 'Z,Z,X',
    [string]$DumpAt = "",        # seconds, comma-separated (one string: powershell -File passes it that way)
    [string[]]$ExtraEnv = @()
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $root "runs\$Label"
New-Item -ItemType Directory -Force $dir | Out-Null
$pagePath = if ([IO.Path]::IsPathRooted($Page)) { $Page } else { Join-Path $root $Page }
if (-not (Test-Path $pagePath)) { throw "no launch page $pagePath" }
$copy = Join-Path $dir 'launch_page.bin'; Copy-Item $pagePath $copy -Force
$frames = Join-Path $dir 'frames.csv'; $ev = Join-Path $dir 'events.csv'
"wall_s,event" | Set-Content $ev
$envs = @("CS_LAUNCH_PAGE=$copy", "RECOMP_CS_FRAME_CSV=$frames", "RECOMP_CS_D3D11_LOG=1", "RECOMP_CS_DUMP=$dir") + $ExtraEnv
$p = @(& (Join-Path $PSScriptRoot 'run.ps1') -Label $Label -ExtraEnv ($envs -join ';') -PassThru) | Where-Object { $_ -is [Diagnostics.Process] } | Select-Object -Last 1
if (-not $p) { throw 'run.ps1 did not return the game process' }
$sw = [Diagnostics.Stopwatch]::StartNew()
function Log([string]$w) { "{0:N1},{1}" -f $sw.Elapsed.TotalSeconds, $w | Add-Content $ev }
function Live { @(Get-Content (Join-Path $dir 'pid.txt') | ForEach-Object { Get-Process -Id ([int]$_) -ErrorAction SilentlyContinue } | Where-Object { $_.ProcessName -eq 'cs2_xbox_recomp' }) }
$keyList = $Keys -split ',' | Where-Object { $_ }
$ki = 0; $nextKey = $KeysFrom; $flag = Join-Path $dir 'dump.flag'
$dumpDone = @{}; $dumpOn = $false; $dumpOff = 0
try {
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
        $live = Live
        if (-not $live) { Log 'exited'; break }
        $t = $sw.Elapsed.TotalSeconds
        if ($t -ge $nextKey -and $keyList) {
            $k = $keyList[$ki % $keyList.Count]; $ki++
            try { & (Join-Path $PSScriptRoot 'send_key.ps1') -ProcessId $live[-1].Id -Key $k | Out-Null; Log "key $k" } catch { Log "key $k failed" }
            $nextKey += $KeyEvery
        }
        # Each dump window: on at the first check past its time, off 2 s later
        # (a slow key send can make one loop iteration take longer than that).
        if ($dumpOn -and $t -ge $dumpOff) { Remove-Item $flag -ErrorAction SilentlyContinue; $dumpOn = $false; Log 'dump off' }
        foreach ($d in ($DumpAt -split "," | Where-Object { $_ } | ForEach-Object { [int]$_ })) {
            if (-not $dumpDone.ContainsKey($d) -and $t -ge $d -and -not $dumpOn) {
                $dumpDone[$d] = 1; New-Item -ItemType File -Force $flag | Out-Null; $dumpOn = $true; $dumpOff = $t + 2; Log "dump on ($d)"
            }
        }
        Start-Sleep -Milliseconds 250
    }
} finally {
    if (Test-Path $flag) { Remove-Item $flag }
    foreach ($q in (Live)) { Stop-Process -Id $q.Id -Force }
    Log 'stopped'
}
& python (Join-Path $PSScriptRoot 'analyze_frames.py') $dir | Select-Object -First 30
