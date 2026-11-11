param([switch]$SkipPython)
# Creates the project's private, pinned xboxrecomp clone and Python deps.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dep = Get-Content (Join-Path $root 'dependencies.json') -Raw | ConvertFrom-Json
$tk = Join-Path $root $dep.xboxrecomp.path
if (-not (Test-Path -LiteralPath (Join-Path $tk '.git'))) {
    & git clone $dep.xboxrecomp.url $tk
    if ($LASTEXITCODE -ne 0) { throw 'git clone xboxrecomp failed' }
}
$item = Get-Item -LiteralPath $tk -Force
if ($item.LinkType) { throw "$tk is a $($item.LinkType); CS2 needs its own clone, not a link." }
& git -C $tk checkout --quiet --detach $dep.xboxrecomp.commit
if ($LASTEXITCODE -ne 0) { throw 'checkout of pinned xboxrecomp commit failed' }
$head = (& git -C $tk rev-parse HEAD).Trim()
if ($head -ne $dep.xboxrecomp.commit) { throw "toolkit HEAD $head != pin $($dep.xboxrecomp.commit)" }
$dirty = & git -C $tk status --porcelain --untracked-files=no
if ($dirty) { throw "toolkit clone has local modifications:`n$dirty" }
Write-Host "xboxrecomp pinned at $head"
if (-not $SkipPython) {
    & python -m pip install --quiet --target (Join-Path $root '.deps') "capstone==$($dep.capstone)"
    if ($LASTEXITCODE -ne 0) { throw 'pip install capstone failed' }
}
