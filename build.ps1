# Builds payload.dll and the injector as 64-bit.
param(
    [string] $Cxx = $env:CXX,
    [string] $JdkHome = $env:JDK_HOME
)

$ErrorActionPreference = "Stop"
$flags = @("-O2", "-s", "-static", "-static-libgcc", "-static-libstdc++")

function Resolve-Compiler {
    param([string] $Requested)

    if ($Requested) {
        $cmd = Get-Command $Requested -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
        if (Test-Path $Requested) { return (Resolve-Path $Requested).Path }
        throw "Requested compiler was not found: $Requested"
    }

    foreach ($name in @("g++", "x86_64-w64-mingw32-g++", "c++")) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }

    $candidates = @(
        "C:/msys64/ucrt64/bin/g++.exe",
        "C:/msys64/ucrt64/bin/x86_64-w64-mingw32-g++.exe",
        "C:/msys64/mingw64/bin/g++.exe",
        "C:/msys64/mingw64/bin/x86_64-w64-mingw32-g++.exe",
        "C:/msys64/clang64/bin/g++.exe",
        "C:/mingw64/bin/g++.exe",
        "C:/w64devkit/bin/g++.exe",
        "C:/WinLibs/mingw64/bin/g++.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
    }

    $searchRoots = @(
        "C:/Program Files/mingw-w64",
        "C:/Program Files/WinLibs",
        "C:/ProgramData/chocolatey/lib/mingw",
        "C:/tools/mingw64",
        "C:/tools/w64devkit",
        "C:/tools/msys64",
        "C:/mingw64"
    )
    foreach ($root in $searchRoots) {
        if (-not (Test-Path $root)) { continue }
        $hit = Get-ChildItem -Path $root -Recurse -Filter "g++.exe" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }

    throw "MinGW-w64 g++ was not found. Add g++ to PATH, run .\build.ps1 -Cxx C:/path/to/g++.exe, or set `$env:CXX."
}

function Invoke-Compiler {
    param(
        [string] $Output,
        [string[]] $Arguments
    )

    & $script:cxx @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Output build failed" }
}

$cxx = Resolve-Compiler $Cxx
Write-Host "Using compiler: $cxx" -ForegroundColor DarkGray

# JDK headers for JNI/JVMTI (payload is a JVMTI agent). Override $env:JDK_HOME if yours differs.
$jdk = if ($JdkHome) { $JdkHome } else { "C:/Program Files/Java/jdk1.8.0_201" }
if (-not (Test-Path "$jdk/include/jvmti.h")) { throw "JVMTI headers not found under $jdk (set `$env:JDK_HOME)" }
$inc = @("-I$jdk/include", "-I$jdk/include/win32")

Write-Host "Building payload.dll ..." -ForegroundColor Cyan
Invoke-Compiler "payload.dll" (@("-shared") + $flags + $inc + @("-o", "payload.dll", "payload.cpp", "-luser32", "-lgdi32", "-lgdiplus", "-lwinmm"))

Write-Host "Building injector.exe ..." -ForegroundColor Cyan
Invoke-Compiler "injector.exe" ($flags + @("-o", "injector.exe", "injector.cpp"))

Write-Host "Done. BotClicker: .\injector.exe" -ForegroundColor Green
