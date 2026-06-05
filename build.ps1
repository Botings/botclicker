# Builds payload.dll and injector.exe as 64-bit (matches modern Minecraft's Java).
$ErrorActionPreference = "Stop"
$flags = @("-O2", "-s", "-static", "-static-libgcc", "-static-libstdc++")

# JDK headers for JNI/JVMTI (payload is a JVMTI agent). Override $env:JDK_HOME if yours differs.
$jdk = if ($env:JDK_HOME) { $env:JDK_HOME } else { "C:/Program Files/Java/jdk1.8.0_201" }
if (-not (Test-Path "$jdk/include/jvmti.h")) { throw "JVMTI headers not found under $jdk (set `$env:JDK_HOME)" }
$inc = @("-I$jdk/include", "-I$jdk/include/win32")

Write-Host "Building payload.dll ..." -ForegroundColor Cyan
& g++ -shared @flags @inc -o payload.dll payload.cpp -luser32 -lgdi32 -lgdiplus -lwinmm
if ($LASTEXITCODE -ne 0) { throw "payload.dll build failed" }

Write-Host "Building injector.exe ..." -ForegroundColor Cyan
& g++ @flags -o injector.exe injector.cpp
if ($LASTEXITCODE -ne 0) { throw "injector.exe build failed" }

Write-Host "Done. Run:  .\injector.exe   (with Minecraft open)" -ForegroundColor Green
