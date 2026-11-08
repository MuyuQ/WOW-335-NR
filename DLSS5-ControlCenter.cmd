@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Dlss5.ControlCenter.ps1"
if errorlevel 1 pause
endlocal
