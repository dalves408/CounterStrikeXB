<#
 Repeated short boots to catch intermittent intro/boot freezes.
   .\scripts\boot_loop.ps1 -Prefix bl -Count 12 [-Seconds 45] [-ExtraEnv 'A=1;B=2']
 Each run is a menu_probe of -Seconds (runs\<Prefix>_<n>; {label} in -ExtraEnv is
 replaced by the run's label); prints one line per
 run: reached the title or not, flips, longest frame gap, and when the last
 frame was. A run that never reaches the title is the freeze.
#>
param(
    [string]$Prefix = 'bl',
    [int]$Count = 12,
    [int]$Seconds = 45,
    [string[]]$ExtraEnv = @()
)
$root = Split-Path $PSScriptRoot -Parent
$results = @()
for ($i = 1; $i -le $Count; $i++) {
    $label = '{0}_{1:D2}' -f $Prefix, $i
    $probe = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'menu_probe.ps1'),
               '-Label', $label, '-Seconds', "$Seconds", '-Steps', 'Down')
    # An empty -ExtraEnv would hand menu_probe a parameter with no value.
    if ($ExtraEnv) { $probe += @('-ExtraEnv', (($ExtraEnv | ForEach-Object { $_ -replace '\{label\}', $label }) -join ';')) }
    $out = & powershell @probe 2>&1 | Out-String
    $title = $out -match 'title screen detected'
    $csv = Join-Path $root "runs\$label\frames.csv"
    $rows = @(if (Test-Path $csv) { Import-Csv $csv })
    $gap = 0; $last = 0
    foreach ($r in $rows) { if ($r.dt_ms) { $d = [double]$r.dt_ms; if ($d -gt $gap) { $gap = $d } }; $last = [double]$r.t_ms }
    $line = '{0}  title={1}  flips={2}  max_gap_ms={3:N0}  last_frame_s={4:N1}' -f $label, $title, $rows.Count, $gap, ($last / 1000)
    Write-Output $line
    $results += $line
}
$frozen = @($results | Where-Object { $_ -match 'title=False' }).Count
Write-Output ("frozen runs: {0} of {1}" -f $frozen, $Count)
