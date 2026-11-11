<#
 Launch the CS2 build from the project root.
   .\scripts\run.ps1                 # normal play: David drives and closes it himself
   .\scripts\run.ps1 -Seconds 90     # bounded diagnostic run (stops only its own PID)
 Logs: runs\<label>\stdout.log, stderr.log, xbox_kernel.log, env.txt
 Save/partition/cache root: SaveData\ (project-local; src/main.c reads CS_SAVE_ROOT).
 Input: real XInput pads + keyboard reach the title as USB XID reports through
 the toolkit's OHCI model (RECOMP_USB=1, RECOMP_KEYBOARD=1).
#>
param(
    [int]$Seconds = 0,
    [string]$Label = ('run_' + (Get-Date -Format 'yyyyMMdd_HHmmss')),
    [int]$FbDumpEvery = 0,           # >0: dump the guest framebuffer every N frames to runs\<label>\fb.bmp
    [switch]$PadLive,                # diagnostics: also feed pad 0 from runs\<label>\pad_live.txt (toolkit RECOMP_PAD_LIVE, same XID report path)
    [string[]]$ExtraEnv = @(),       # NAME=VALUE pairs for experiments (';'-separated ok)
    [switch]$PassThru
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root 'build\RelWithDebInfo\cs2_xbox_recomp.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "missing $exe; run scripts\build.ps1" }
if ($Label -notmatch '^[A-Za-z0-9_\-]+$') { throw 'label: letters, digits, - and _ only' }
$dir = Join-Path $root "runs\$Label"
New-Item -ItemType Directory -Force $dir | Out-Null

# Start from a clean slate: inherited RECOMP_*/CS_* knobs from another
# project's shell silently change behaviour (and RECOMP_SAVE_DIR moves saves).
Get-ChildItem env: | Where-Object { $_.Name -match '^(RECOMP_|CS_)' } |
    ForEach-Object { Remove-Item "env:$($_.Name)" }

$envs = [ordered]@{
    RECOMP_USB         = '1'   # OHCI + USB gamepad model (XAPI's real input path)
    RECOMP_KEYBOARD    = '1'   # keyboard -> pad 0 (toolkit map: arrows=D-pad, Enter=START, Backspace=BACK, Z/X/C/V=A/B/X/Y, WASD=left stick)
    RECOMP_FB_WINDOW   = '1'
    RECOMP_D3D_PRESENT = '1'
    CS_SAVE_ROOT       = (Join-Path $root 'SaveData')   # partition images / cache (src/main.c)
    RECOMP_SAVE_DIR    = (Join-Path $root 'SaveData')   # TDATA/UDATA (profiles, saves)
    # Required for boot (runs/b1_long hangs without them; verified p1_pvideo):
    RECOMP_PB_EXEC            = '1'       # toolkit NV2A pushbuffer executor (draws the D3D frames)
    RECOMP_CONTIG_AVOID_IMAGE = '1'       # contiguous allocations placed above the XBE image
    RECOMP_AC97_READY         = '1'       # AC97 codec reports ready (DSOUND init)
    RECOMP_VBLANK             = 'paced'   # paced 60 Hz vblank interrupts
}
if ($FbDumpEvery -gt 0) {
    $envs['RECOMP_FB_DUMP'] = (Join-Path $dir 'fb.bmp')
    $envs['RECOMP_FB_WINDOW_DUMP_EVERY'] = "$FbDumpEvery"
}
if ($PadLive) {
    $live = Join-Path $dir 'pad_live.txt'; New-Item -ItemType File -Force $live | Out-Null
    $envs['RECOMP_PAD_LIVE'] = $live
    $envs['RECOMP_INPUT_DIAG'] = '1'
}
# ';'-separated too, because powershell -File passes a comma list as one string.
foreach ($kv in (($ExtraEnv -join ';') -split ';' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; $envs[$k] = $v }
foreach ($k in $envs.Keys) { [Environment]::SetEnvironmentVariable($k, $envs[$k], 'Process') }
($envs.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) | Set-Content (Join-Path $dir 'env.txt')

$p = Start-Process -FilePath $exe -WorkingDirectory $root -PassThru `
    -RedirectStandardOutput (Join-Path $dir 'stdout.log') -RedirectStandardError (Join-Path $dir 'stderr.log')
"$($p.Id)" | Set-Content (Join-Path $dir 'pid.txt')
Write-Host "PID $($p.Id); logs in $dir"
if ($PassThru) { return $p }
if ($Seconds -gt 0) {
    if (-not $p.WaitForExit($Seconds * 1000)) {
        Stop-Process -Id $p.Id -Force
        $p.WaitForExit(5000) | Out-Null
        Write-Host "stopped PID $($p.Id) after $Seconds s"
    } else { Write-Host "exited with $($p.ExitCode)" }
    $klog = Join-Path $root 'xbox_kernel.log'
    if (Test-Path $klog) { Copy-Item $klog (Join-Path $dir 'xbox_kernel.log') -Force }
}
