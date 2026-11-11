param([switch]$SkipPython)
# Creates the project's private, pinned xboxrecomp clone and Python deps.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dep = Get-Content (Join-Path $root 'dependencies.json') -Raw | ConvertFrom-Json
$tk = Join-Path $root $dep.xboxrecomp.path
if (-not (Test-Path -LiteralPath (Join-Path $tk '.git'))) {
    # autocrlf off: git am of the local patches fails on a CRLF-converted checkout.
    & git clone -c core.autocrlf=false $dep.xboxrecomp.url $tk
    if ($LASTEXITCODE -ne 0) { throw 'git clone xboxrecomp failed' }
}
$item = Get-Item -LiteralPath $tk -Force
if ($item.LinkType) { throw "$tk is a $($item.LinkType); CS2 needs its own clone, not a link." }
# commit = base_commit + local_patches (git am with a fixed identity, so the
# resulting sha is reproducible). Reuse it if this clone already has it.
& git -C $tk checkout --quiet --detach $dep.xboxrecomp.commit 2>$null
if ($LASTEXITCODE -ne 0) {
    & git -C $tk checkout --quiet --detach $dep.xboxrecomp.base_commit
    if ($LASTEXITCODE -ne 0) { throw 'checkout of base xboxrecomp commit failed' }
    $env:GIT_COMMITTER_NAME = 'CounterStrikeXB2'; $env:GIT_COMMITTER_EMAIL = 'cs2@local'
    $env:GIT_COMMITTER_DATE = '2026-11-11T06:00:00-08:00'
    foreach ($p in $dep.xboxrecomp.local_patches) {
        & git -C $tk -c core.autocrlf=false am --quiet --committer-date-is-author-date (Join-Path $root $p)
        if ($LASTEXITCODE -ne 0) { & git -C $tk am --abort; throw "local patch failed: $p" }
    }
}
$head = (& git -C $tk rev-parse HEAD).Trim()
if ($head -ne $dep.xboxrecomp.commit) { throw "toolkit HEAD $head != pin $($dep.xboxrecomp.commit)" }
$dirty = & git -C $tk status --porcelain --untracked-files=no
if ($dirty) { throw "toolkit clone has local modifications:`n$dirty" }
Write-Host "xboxrecomp pinned at $head"
if (-not $SkipPython) {
    & python -m pip install --quiet --target (Join-Path $root '.deps') "capstone==$($dep.capstone)"
    if ($LASTEXITCODE -ne 0) { throw 'pip install capstone failed' }
}
