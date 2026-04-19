@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..\..") do set "REPO_ROOT=%%~fI"

set "TARGET=%REPO_ROOT%\build\Debug\Quantum Miner Control Center.exe"
if not exist "%TARGET%" set "TARGET=%REPO_ROOT%\build\Release\Quantum Miner Control Center.exe"

if not exist "%TARGET%" (
    echo Genesis Control Center executable not found.
    echo Expected either:
    echo   %REPO_ROOT%\build\Debug\Quantum Miner Control Center.exe
    echo   %REPO_ROOT%\build\Release\Quantum Miner Control Center.exe
    exit /b 1
)

start "" /D "%REPO_ROOT%" "%TARGET%"