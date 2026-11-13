Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$project = [IO.Path]::GetFullPath((Join-Path $repo '../..'))
$output = Join-Path $project ('_scratch/dlss5-aio/native-test-' + [guid]::NewGuid().ToString('N'))
$current = $output
while ($current) {
    $item = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
    if ($null -ne $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Test path contains a link.' }
    $current = [IO.Path]::GetDirectoryName($current)
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vs) { throw 'C++ build tools not found.' }
[IO.Directory]::CreateDirectory($output) | Out-Null
$names = @('NR_TEST_VSDEV','NR_TEST_CPP','NR_TEST_PANEL','NR_TEST_IMGUI','NR_TEST_OUTPUT')
$prior = @{}
foreach ($name in $names) { $prior[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
try {
    $env:NR_TEST_VSDEV = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
    $env:NR_TEST_CPP = Join-Path $PSScriptRoot 'native_regression.cpp'
    $env:NR_TEST_PANEL = Join-Path $PSScriptRoot 'panel_regression.cpp'
    $env:NR_TEST_IMGUI = Join-Path $repo 'native/imgui'
    $env:NR_TEST_OUTPUT = $output
    $body = @'
@echo off
call "%NR_TEST_VSDEV%" -no_logo -arch=x64 >nul
if errorlevel 1 exit /b 1
cd /d "%NR_TEST_OUTPUT%"
cl /nologo /std:c++17 /EHsc /W4 /utf-8 "%NR_TEST_CPP%" /Fe:native-regression.exe /link d3d12.lib user32.lib
if errorlevel 1 exit /b 1
native-regression.exe "%NR_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /Gy /DUNICODE /D_UNICODE /utf-8 /I"%NR_TEST_IMGUI%" "%NR_TEST_PANEL%" "%NR_TEST_IMGUI%\imgui.cpp" "%NR_TEST_IMGUI%\imgui_draw.cpp" "%NR_TEST_IMGUI%\imgui_tables.cpp" "%NR_TEST_IMGUI%\imgui_widgets.cpp" "%NR_TEST_IMGUI%\backends\imgui_impl_dx11.cpp" "%NR_TEST_IMGUI%\backends\imgui_impl_win32.cpp" /Fe:panel-regression.exe /link /OPT:REF d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib advapi32.lib imm32.lib shell32.lib
if errorlevel 1 exit /b 1
panel-regression.exe "%NR_TEST_OUTPUT%"
exit /b %ERRORLEVEL%
'@
    $cmd = Join-Path $output 'run.cmd'
    [IO.File]::WriteAllText($cmd,$body,[Text.Encoding]::ASCII)
    & $env:ComSpec /d /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "Native regression failed; evidence: $output" }
    Write-Output "Native regression passed; evidence: $output"
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$prior[$name],'Process') }
}
