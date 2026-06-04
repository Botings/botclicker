# Builds payload.dll and injector.exe as 64-bit (matches modern Minecraft's Java).
$ErrorActionPreference = "Stop"
$flags = @("-O2", "-s", "-static", "-static-libgcc", "-static-libstdc++")

Write-Host "Building payload.dll ..." -ForegroundColor Cyan
& g++ -shared @flags -o payload.dll payload.cpp -luser32
if ($LASTEXITCODE -ne 0) { throw "payload.dll build failed" }

Write-Host "Building injector.exe ..." -ForegroundColor Cyan
& g++ @flags -o injector.exe injector.cpp
if ($LASTEXITCODE -ne 0) { throw "injector.exe build failed" }

Write-Host "Done. Run:  .\injector.exe   (with Minecraft open)" -ForegroundColor Green
