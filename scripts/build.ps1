param([ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dep = Get-Content (Join-Path $root 'dependencies.json') -Raw | ConvertFrom-Json
$tk = Join-Path $root $dep.xboxrecomp.path
$head = (& git -C $tk rev-parse HEAD).Trim()
if ($head -ne $dep.xboxrecomp.commit) { throw "toolkit at $head, pin is $($dep.xboxrecomp.commit); run scripts/setup.ps1" }
if (& git -C $tk status --porcelain --untracked-files=no) { throw 'toolkit clone has local modifications' }
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
$build = Join-Path $root 'build'
& $cmake -S $root -B $build -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& $cmake --build $build --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'CMake build failed' }
Write-Host "Built $Configuration -> $build\$Configuration\cs2_xbox_recomp.exe"
