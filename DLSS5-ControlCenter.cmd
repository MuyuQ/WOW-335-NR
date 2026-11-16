@echo off
setlocal
rem zh: 父进程若来自 PowerShell 7，PSModulePath 会指向 PS7 模块目录，
rem Windows PowerShell 5.1 无法自动加载 Utility/CimCmdlets，表现为
rem 「无法将 Get-FileHash 项识别为 cmdlet」。这里把 5.1 系统模块路径前置。
set "PSModulePath=%SystemRoot%\System32\WindowsPowerShell\v1.0\Modules;%ProgramFiles%\WindowsPowerShell\Modules;%PSModulePath%"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0Dlss5.ControlCenter.ps1"
if errorlevel 1 pause
endlocal
