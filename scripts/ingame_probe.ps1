<#
 Reach Airstrip with a key schedule, dismiss in-game tutorial/buy overlays with
 periodic Z (= A on the pad), and briefly pulse dump.flag so RECOMP_CS_DUMP
 captures RTs. Enter/Return is only used for the pre-map menu path (START).
   .\scripts\ingame_probe.ps1 -Label d7_clean -Seconds 400
   .\scripts\ingame_probe.ps1 -Label d8_cull0 -Seconds 400 -ExtraEnv 'RECOMP_CS_CULL=0'
 Does not edit probe.ps1 / run.ps1; only wraps them.
#>
param(
    [Parameter(Mandatory)][string]$Label,
    [int]$Seconds = 400,
    [string]$Keys = '',
    [string]$ExtraEnv = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $root "runs\$Label"
New-Item -ItemType Directory -Force $dir | Out-Null

# Menu path (docs/HANDOFF.md): title START with Return, Main Menu Down x1 to
# Single Player, then A (Z) on every screen (Beginner, Airstrip, confirms).
# In-map: Z (= A) dismisses tips/buy, X (= B) a second later closes an A-opened
# weapon radial before dump frames.
if (-not $Keys) {
    $Keys = @(
        '25:Return'
        '33:Return'
        '45:Return'
        '55:Down'
        '58:Z'
        '64:Z'
        '70:Z'
        '76:Z'
        '82:Z'
        '90:Z'
        '200:Z'
        '201:X'
        '206:Z'
        '207:X'
        '212:Z'
        '213:X'
        '218:Z'
        '219:X'
        '224:Z'
        '225:X'
        '230:Z'
        '231:X'
        '236:Z'
        '237:X'
        '242:Z'
        '243:X'
        '248:Z'
        '249:X'
        '254:Z'
        '255:X'
        '260:Z'
        '261:X'
        '266:Z'
        '267:X'
        '272:Z'
        '273:X'
        '278:Z'
        '279:X'
        '284:Z'
        '285:X'
        '290:Z'
        '291:X'
        '296:Z'
        '297:X'
        '302:Z'
        '303:X'
        '308:Z'
        '309:X'
        '314:Z'
        '315:X'
        '320:Z'
        '321:X'
        '326:Z'
        '327:X'
        '332:Z'
        '333:X'
        '338:Z'
        '339:X'
        '344:Z'
        '345:X'
        '350:Z'
        '351:X'
        '356:Z'
        '357:X'
        '362:Z'
        '363:X'
        '368:Z'
        '369:X'
    ) -join ';'
}

# Last in-map dismiss (~368 s) → pulse dump.flag ~20 s later, then again 30 s later.
$parts = $Keys -split ';' | Where-Object { $_ }
$lastDismiss = 0
foreach ($p in $parts) {
    $t, $n = $p -split ':', 2
    if (($n -eq 'Z' -or $n -eq 'X' -or $n -eq 'Return') -and [double]$t -gt $lastDismiss) { $lastDismiss = [double]$t }
}
$dumpDelays = @([int]($lastDismiss + 20), [int]($lastDismiss + 50))
$jobs = foreach ($delay in $dumpDelays) {
    if ($delay + 2 -gt $Seconds) { continue }
    Start-Job -ScriptBlock {
        param($dir, $delay)
        Start-Sleep -Seconds $delay
        $flag = Join-Path $dir 'dump.flag'
        New-Item -ItemType File -Path $flag -Force | Out-Null
        Start-Sleep -Seconds 2
        Remove-Item -LiteralPath $flag -Force -ErrorAction SilentlyContinue
    } -ArgumentList $dir, $delay
}

$envPairs = @('RECOMP_CS_D3D11_LOG=1', "RECOMP_CS_DUMP=$dir")
if ($ExtraEnv) {
    foreach ($kv in ($ExtraEnv -split ';' | Where-Object { $_ })) { $envPairs += $kv }
}

try {
    & (Join-Path $PSScriptRoot 'probe.ps1') `
        -Label $Label `
        -Seconds $Seconds `
        -Keys $Keys `
        -ExtraEnv ($envPairs -join ';')
}
finally {
    Remove-Item -LiteralPath (Join-Path $dir 'dump.flag') -Force -ErrorAction SilentlyContinue
    if ($jobs) {
        $jobs | Wait-Job -Timeout 5 | Out-Null
        $jobs | Remove-Job -Force -ErrorAction SilentlyContinue
    }
}
