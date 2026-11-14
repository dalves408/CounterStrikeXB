<#
 Straight into a match with a given settings.ini, window captures and keys on a
 schedule -- for checking the F1 / config.exe settings and the ESC/F1 menus.
   .\scripts\settings_probe.ps1 -Label st1 -Settings 'resolution_scale=2;msaa=4' `
       -Steps '40:shot:game;42:key:F1;44:shot:menu;46:key:Down;47:shot:menu2' [-Seconds 60]
 The settings go into runs\<label>\appdata\CounterStrikeXB\settings.ini, and
 LOCALAPPDATA points there for this run only: the player's own settings are
 never read or written. Steps: <seconds>:shot:<name> (scripts/winshot.py ->
 runs\<label>\<name>.png) or <seconds>:key:<key> (scripts/send_key.ps1).
#>
param(
    [Parameter(Mandatory)][string]$Label,
    [string]$Settings = '',
    [string]$Steps = '',
    [int]$Seconds = 60,
    [string]$Page = 'runs\launch_page_dust.bin',
    [string]$ExtraEnv = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $root "runs\$Label"
$app = Join-Path $dir 'appdata'
New-Item -ItemType Directory -Force (Join-Path $app 'CounterStrikeXB') | Out-Null
$ini = Join-Path $app 'CounterStrikeXB\settings.ini'
$lines = @('[display]') + @($Settings -split ';' | Where-Object { $_ })
$lines | Set-Content -Encoding ascii $ini
$frames = Join-Path $dir 'frames.csv'
Remove-Item -LiteralPath $frames -ErrorAction SilentlyContinue
$envs = @("RECOMP_CS_FRAME_CSV=$frames", "RECOMP_CS_D3D11_LOG=1", "LOCALAPPDATA=$app")
if ($Page) {   # -Page '' boots normally (intros, menus)
    $pagePath = if ([IO.Path]::IsPathRooted($Page)) { $Page } else { Join-Path $root $Page }
    $copy = Join-Path $dir 'launch_page.bin'; Copy-Item $pagePath $copy -Force
    $envs += "CS_LAUNCH_PAGE=$copy"
}
if ($ExtraEnv) { $envs += @($ExtraEnv -split ';' | Where-Object { $_ }) }
$p = @(& (Join-Path $PSScriptRoot 'run.ps1') -Label $Label -ExtraEnv ($envs -join ';') -PassThru) | Where-Object { $_ -is [Diagnostics.Process] } | Select-Object -Last 1
if (-not $p) { throw 'run.ps1 did not return the game process' }
$sched = foreach ($s in ($Steps -split ';' | Where-Object { $_ })) {
    $t, $kind, $arg = $s -split ':', 3
    [pscustomobject]@{ T = [double]$t; Kind = $kind; Arg = $arg; Done = $false }
}
$sw = [Diagnostics.Stopwatch]::StartNew()
function Live { @(Get-Content (Join-Path $dir 'pid.txt') | ForEach-Object { Get-Process -Id ([int]$_) -ErrorAction SilentlyContinue } | Where-Object { $_.ProcessName -eq 'cs2_xbox_recomp' }) }
try {
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
        $live = Live
        if (-not $live) { "exited at $([int]$sw.Elapsed.TotalSeconds) s"; break }
        $t = $sw.Elapsed.TotalSeconds
        foreach ($s in $sched) {
            if ($s.Done -or $t -lt $s.T) { continue }
            $s.Done = $true
            if ($s.Kind -eq 'shot') {
                & python -I (Join-Path $PSScriptRoot 'winshot.py') (Join-Path $dir "$($s.Arg).png") $live[-1].Id
            } elseif ($s.Kind -eq 'key') {
                & (Join-Path $PSScriptRoot 'send_key.ps1') -ProcessId $live[-1].Id -Key $s.Arg | Out-Null
                "key $($s.Arg) at $([int]$t) s"
            }
        }
        Start-Sleep -Milliseconds 200
    }
} finally {
    foreach ($q in (Live)) { Stop-Process -Id $q.Id -Force }
}
Get-Content (Join-Path $dir 'stderr.log') | Select-String 'CS-SET|render scale|presenting|swap chain' | ForEach-Object Line
& python (Join-Path $PSScriptRoot 'analyze_frames.py') $dir | Select-String 'scene |median|stalls'
