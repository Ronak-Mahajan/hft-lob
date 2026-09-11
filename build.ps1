# Build script: finds g++ (PATH first, then the WinGet WinLibs install) and
# compiles all three binaries. Usage:  .\build.ps1 [-Run] [-Quick]
#   -Run    run lob_bench.exe, lob_parallel.exe and spsc_stress.exe after building
#   -Quick  pass --quick to the benchmarks (CI sizes; not a measurement)
param([switch]$Run, [switch]$Quick)

$gxx = (Get-Command g++ -ErrorAction SilentlyContinue).Source
if (-not $gxx) {
    $winlibs = "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe"
    if (Test-Path $winlibs) { $gxx = $winlibs }
    else { Write-Error "g++ not found. Install with: winget install BrechtSanders.WinLibs.POSIX.UCRT"; exit 1 }
}

$root = $PSScriptRoot
$flags = @("-std=c++20", "-O3", "-march=native", "-DNDEBUG", "-Wall", "-Wextra", "-pthread", "-static", "-I", "$root\include")

# main.cpp: unit checks, differential fuzz, latency percentiles, single-core throughput
& $gxx @flags "$root\src\main.cpp" -o "$root\lob_bench.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "built: $root\lob_bench.exe"

# parallel_main.cpp: parallel-vs-sequential verification and the multi-core scaling tables
& $gxx @flags "$root\src\parallel_main.cpp" -o "$root\lob_parallel.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "built: $root\lob_parallel.exe"

# spsc_stress.cpp: sequence-checked SPSC ring stress (the ThreadSanitizer target in CI)
& $gxx @flags "$root\src\spsc_stress.cpp" -o "$root\spsc_stress.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "built: $root\spsc_stress.exe"

if ($Run) {
    $arg = @()                       # stays an array even with one element
    if ($Quick) { $arg += "--quick" }
    & "$root\lob_bench.exe" @arg
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & "$root\lob_parallel.exe" @arg
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & "$root\spsc_stress.exe"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
