@echo off
rem mkgp2-pc rendering bring-up for 32-bit Windows (a window, sound, keyboard: KEYS.txt).
rem Usage: run-gfx.cmd "D:\path\to\Mario Kart Arcade GP 2 (Japan).iso"
rem The log goes to gfx.log next to this file.
if "%~1"=="" (
  echo usage: %~nx0 "path to the ISO"
  exit /b 1
)
set "MKGP2_ISO=%~1"
cd /d "%~dp0"
"%~dp0mkgp2-gfx.exe" > "%~dp0gfx.log" 2>&1
echo exit code %ERRORLEVEL% >> "%~dp0gfx.log"
echo finished, log: %~dp0gfx.log
