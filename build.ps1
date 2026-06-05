# Builds payload.dll, injector.exe, and password.exe as 64-bit.
$ErrorActionPreference = "Stop"
$flags = @("-O2", "-s", "-static", "-static-libgcc", "-static-libstdc++")

# JDK headers for JNI/JVMTI (payload is a JVMTI agent). Override $env:JDK_HOME if yours differs.
$jdk = if ($env:JDK_HOME) { $env:JDK_HOME } else { "C:/Program Files/Java/jdk1.8.0_201" }
if (-not (Test-Path "$jdk/include/jvmti.h")) { throw "JVMTI headers not found under $jdk (set `$env:JDK_HOME)" }
$inc = @("-I$jdk/include", "-I$jdk/include/win32")

Write-Host "Building payload.dll ..." -ForegroundColor Cyan
& g++ -shared @flags @inc -o payload.dll payload.cpp -luser32 -lgdi32 -lgdiplus -lwinmm -lbcrypt
if ($LASTEXITCODE -ne 0) { throw "payload.dll build failed" }

Write-Host "Building injector.exe ..." -ForegroundColor Cyan
& g++ @flags -o injector.exe injector.cpp
if ($LASTEXITCODE -ne 0) { throw "injector.exe build failed" }

Write-Host "Building password.exe ..." -ForegroundColor Cyan
& g++ @flags -o password.exe password.cpp -lbcrypt
if ($LASTEXITCODE -ne 0) { throw "password.exe build failed" }

Write-Host "Done. Run:  .\password.exe   then .\injector.exe (with Minecraft open)" -ForegroundColor Green
