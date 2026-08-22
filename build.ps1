# Build script: finds g++ (PATH first, then the WinGet WinLibs install) and
# compiles the benchmark. Usage:  .\build.ps1 [-Run]
param([switch]$Run)

$gxx = (Get-Command g++ -ErrorAction SilentlyContinue).Source
if (-not $gxx) {
    $winlibs = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe"
    if (Test-Path $winlibs) { $gxx = $winlibs }
    else { Write-Error "g++ not found. Install with: winget install BrechtSanders.WinLibs.POSIX.UCRT"; exit 1 }
}

$root = $PSScriptRoot
& $gxx -std=c++20 -O3 -march=native -DNDEBUG -Wall -Wextra -static `
    -I "$root\include" "$root\src\main.cpp" -o "$root\lob_bench.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "built: $root\lob_bench.exe"
if ($Run) { & "$root\lob_bench.exe" }
