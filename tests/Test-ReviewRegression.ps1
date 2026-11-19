param([string]$VisualStudioPath = 'C:\Program Files\Microsoft Visual Studio\2022\Community')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$scratch = [IO.Path]::GetFullPath((Join-Path $repo '../../_scratch'))
$output = Join-Path $scratch ('dlss5-aio/review-regression-' + [guid]::NewGuid().ToString('N'))
$current = $output
while ($current) {
    $item = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
    if ($null -ne $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Test path contains a link.' }
    $current = [IO.Path]::GetDirectoryName($current)
}
[IO.Directory]::CreateDirectory($output) | Out-Null
[IO.Directory]::CreateDirectory((Join-Path $output 'x86')) | Out-Null
$vsdev = Join-Path $VisualStudioPath 'Common7/Tools/VsDevCmd.bat'
if (-not [IO.File]::Exists($vsdev)) { throw 'VS C++ tools unavailable.' }
$body = @'
@echo off
cd /d "%NR_REVIEW_OUT%"
call "%NR_REVIEW_VS%" -no_logo -arch=x64 >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /W4 /WX /utf-8 "%NR_REVIEW_REPO%\tests\pipe_io.cpp" /Fe:pipe-io.exe
if errorlevel 1 exit /b 1
pipe-io.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /W4 /WX /utf-8 "%NR_REVIEW_REPO%\tests\control_center_config_wtf.cpp" /Fe:config-wtf.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
config-wtf.exe "%NR_REVIEW_OUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /W4 /WX /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 /I"%NR_REVIEW_REPO%\native\control_center" "%NR_REVIEW_REPO%\tests\control_center_app.cpp" "%NR_REVIEW_REPO%\native\control_center\app\cc_pages.cpp" /Fe:app-regression.exe /link user32.lib gdi32.lib d2d1.lib dwrite.lib bcrypt.lib dxgi.lib ole32.lib oleaut32.lib shell32.lib uuid.lib
if errorlevel 1 exit /b 1
app-regression.exe "%NR_REVIEW_OUT%" "%NR_REVIEW_REPO%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /Gy /MT /utf-8 /DUNICODE /D_UNICODE /I"%NR_REVIEW_REPO%\native\imgui" "%NR_REVIEW_REPO%\tests\console_regression.cpp" "%NR_REVIEW_REPO%\native\imgui\imgui.cpp" "%NR_REVIEW_REPO%\native\imgui\imgui_draw.cpp" "%NR_REVIEW_REPO%\native\imgui\imgui_tables.cpp" "%NR_REVIEW_REPO%\native\imgui\imgui_widgets.cpp" "%NR_REVIEW_REPO%\native\imgui\backends\imgui_impl_dx11.cpp" "%NR_REVIEW_REPO%\native\imgui\backends\imgui_impl_win32.cpp" /Fe:console-regression.exe /link /OPT:REF user32.lib gdi32.lib advapi32.lib d3d11.lib dxgi.lib d3dcompiler.lib imm32.lib shell32.lib shlwapi.lib ole32.lib uuid.lib
if errorlevel 1 exit /b 1
console-regression.exe "%NR_REVIEW_OUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /W4 /WX /utf-8 "%NR_REVIEW_REPO%\tests\wrapper_teardown_policy.cpp" /Fe:wrapper-teardown-policy.exe
if errorlevel 1 exit /b 1
wrapper-teardown-policy.exe
if errorlevel 1 exit /b 1
call "%NR_REVIEW_VS%" -no_logo -arch=x86 >nul
if errorlevel 1 exit /b 1
cd x86
cl /nologo /utf-8 /LD /EHsc /O2 /MT /W3 /std:c++20 /I"%NR_REVIEW_REPO%\carrier\x86-host\src" /I"%NR_REVIEW_REPO%\carrier\addon\include" /I"%NR_REVIEW_REPO%\carrier\external\DLSS5-Feeder\src" /I"%NR_REVIEW_REPO%\carrier\external\DLSS5-Feeder\external\reshade\include" /I"%NR_REVIEW_REPO%\carrier\external\DLSS5-Feeder\external\imgui" /I"%NR_REVIEW_REPO%\carrier\external\DLSS5-Feeder\external\vulkan" /I"%NR_REVIEW_REPO%\carrier\external\DLSS5-Feeder\external\minhook\include" "%NR_REVIEW_REPO%\carrier\x86-host\src\aio-wrapper32.cpp" "%NR_REVIEW_REPO%\carrier\external\DLSS5-Feeder\external\minhook\src\buffer.c" "%NR_REVIEW_REPO%\carrier\external\DLSS5-Feeder\external\minhook\src\hook.c" "%NR_REVIEW_REPO%\carrier\external\DLSS5-Feeder\external\minhook\src\trampoline.c" "%NR_REVIEW_REPO%\carrier\external\DLSS5-Feeder\external\minhook\src\hde\hde32.c" /link /OUT:standalone-dlssnr.addon32 d3d9.lib d3d10_1.lib d3d11.lib dxgi.lib kernel32.lib user32.lib advapi32.lib
exit /b %ERRORLEVEL%
'@
$cmd = Join-Path $output 'run.cmd'
[IO.File]::WriteAllText($cmd, $body, [Text.Encoding]::ASCII)
$names = @('NR_REVIEW_OUT', 'NR_REVIEW_REPO', 'NR_REVIEW_VS')
$old = @{}
foreach ($name in $names) { $old[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    $env:NR_REVIEW_OUT = $output; $env:NR_REVIEW_REPO = $repo; $env:NR_REVIEW_VS = $vsdev
    & $env:ComSpec /d /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "Review regression failed: $output" }
    Write-Output "Review regression passed: $output"
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $old[$name], 'Process') }
}
