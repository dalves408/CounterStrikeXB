<#
 Reactive front-end probe: boot, let the intro movies play, wait until the
 title screen is really up, press START, then walk the menus with a key script.
   .\scripts\menu_probe.ps1 -Label m2 [-Seconds 120] [-Steps 'Down,Z,X,...'] [-StepMs 1500] [-ExtraEnv 'A=1;B=2']
 Detection uses the game's own timing logs (src/cs_d3d11.c, src/cs_gpu.c):
   title = intro movies seen and quiet for 2 s, and the last 60 flips drew exactly 2 batches
   menu  = the last 30 flips drew more than 2 batches
 Writes runs\<label>\frames.csv, pvideo.csv, events.csv (t_ms on the game's clock
 when available) and runs scripts\analyze_frames.py at the end. Stops only its PIDs.
#>
param(
    [Parameter(Mandatory)][string]$Label,
    [int]$Seconds = 120,
    [string]$Steps = 'Down,Down,Up,Up,Z,X,Down,Z,X,Down,Down,Down,Up,Up,Up',
    [int]$StepMs = 1500,
    [string[]]$ExtraEnv = @(),
    [switch]$NoIntroWait
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $root "runs\$Label"
New-Item -ItemType Directory -Force $dir | Out-Null
$frames = Join-Path $dir 'frames.csv'; $pv = Join-Path $dir 'pvideo.csv'; $ev = Join-Path $dir 'events.csv'
"wall_s,game_ms,event" | Set-Content $ev
$envs = @("RECOMP_CS_FRAME_CSV=$frames", "RECOMP_CS_PVIDEO_CSV=$pv", "RECOMP_CS_D3D11_LOG=1") + $ExtraEnv
$p = @(& (Join-Path $PSScriptRoot 'run.ps1') -Label $Label -ExtraEnv ($envs -join ';') -PassThru) | Where-Object { $_ -is [Diagnostics.Process] } | Select-Object -Last 1
if (-not $p) { throw 'run.ps1 did not return the game process' }
$sw = [Diagnostics.Stopwatch]::StartNew()

function Tail-Frames([int]$n) {
    if (-not (Test-Path $frames)) { return @() }
    try { $lines = Get-Content $frames -Tail $n -ErrorAction Stop } catch { return @() }
    $rows = foreach ($l in @($lines)) { if (-not $l) { continue }; $c = $l.Split(','); if ($c.Count -ge 4 -and $c[0] -ne 't_ms') { [pscustomobject]@{ t = [double]$c[0]; draws = [int]$c[3] } } }
    return @($rows)
}
function Game-Ms { $r = @(Tail-Frames 3); if ($r.Count -gt 0) { return $r[-1].t } else { return -1 } }
function Last-Movie-Ms {
    if (-not (Test-Path $pv)) { return -1 }
    try { $l = Get-Content $pv -Tail 1 -ErrorAction Stop } catch { return -1 }
    if (-not $l) { return -1 }
    $c = "$l".Split(','); if ($c[0] -eq 't_ms') { return -1 }; return [double]$c[0]
}
function Log-Event([string]$what) { "{0:N1},{1},{2}" -f $sw.Elapsed.TotalSeconds, (Game-Ms), $what | Add-Content $ev; Write-Host ("{0,6:N1}s  {1}" -f $sw.Elapsed.TotalSeconds, $what) }
function Alive { -not $p.HasExited }
function Send([string]$k) {
    try { & (Join-Path $PSScriptRoot 'send_key.ps1') -ProcessId $p.Id -Key $k | Out-Null; Log-Event "key $k" }
    catch { Log-Event "key $k FAILED $_" }
}

try {
# 1. title
$state = 'boot'
while ($sw.Elapsed.TotalSeconds -lt $Seconds -and (Alive)) {
    Start-Sleep -Milliseconds 250
    $g = Game-Ms; $m = Last-Movie-Ms; $r = @(Tail-Frames 60)
    if ($state -eq 'boot' -and $m -ge 0) { $state = 'movies'; Log-Event 'intro movie started' }
    # Title: the intro movies were seen and have been quiet for 2 s, and the last 60
    # flips drew exactly the title's two batches (background + resolve). -NoIntroWait
    # (or a boot that never shows a movie for 40 s) drops the movie condition.
    $movieOk = ($m -ge 0 -and $g - $m -gt 2000) -or ($m -lt 0 -and ($NoIntroWait -or $sw.Elapsed.TotalSeconds -gt 40))
    if ($r.Count -ge 60 -and $g -gt 0 -and $movieOk -and -not ($r | Where-Object { $_.draws -ne 2 })) {
        Log-Event 'title screen detected'; break
    }
}
# 2. START until the menu workload appears (a few tries: the title can ignore an early press)
for ($i = 0; $i -lt 5 -and (Alive) -and $sw.Elapsed.TotalSeconds -lt $Seconds; $i++) {
    Send 'Return'
    $t0 = $sw.Elapsed.TotalSeconds
    while ($sw.Elapsed.TotalSeconds - $t0 -lt 4) {
        Start-Sleep -Milliseconds 250
        $r = @(Tail-Frames 30)
        if ($r.Count -ge 30 -and -not ($r | Where-Object { $_.draws -le 2 })) { break }
    }
    $r = @(Tail-Frames 30)
    if ($r.Count -ge 30 -and -not ($r | Where-Object { $_.draws -le 2 })) { Log-Event 'main menu detected'; break }
}
# 3. menu walk
foreach ($k in ($Steps -split ',' | Where-Object { $_ })) {
    if (-not (Alive) -or $sw.Elapsed.TotalSeconds -ge $Seconds) { break }
    Start-Sleep -Milliseconds $StepMs
    Send $k.Trim()
}
while ((Alive) -and $sw.Elapsed.TotalSeconds -lt $Seconds) { Start-Sleep -Milliseconds 500 }
} finally {
foreach ($id in (Get-Content (Join-Path $dir 'pid.txt'))) {
    $q = Get-Process -Id ([int]$id) -ErrorAction SilentlyContinue
    if ($q -and $q.ProcessName -eq 'cs2_xbox_recomp') { Stop-Process -Id $q.Id -Force }
}
Log-Event 'stopped'
}
Start-Sleep -Milliseconds 500
& python (Join-Path $PSScriptRoot 'analyze_frames.py') $dir
