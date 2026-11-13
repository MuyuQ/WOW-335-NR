Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$project = [IO.Path]::GetFullPath((Join-Path $repo '../..'))
$output = Join-Path $project ('_scratch/dlss5-aio/shutdown-test-' + [guid]::NewGuid().ToString('N'))
$current = $output
while ($current) {
    $item = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
    if ($null -ne $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Test output contains a link.' }
    $current = [IO.Path]::GetDirectoryName($current)
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vs) { throw 'C++ build tools unavailable.' }
[IO.Directory]::CreateDirectory($output) | Out-Null
$names = @('NR_R01_REPO','NR_R01_OUTPUT','NR_R01_VSDEV','LOCALAPPDATA')
$prior = @{}
foreach ($name in $names) { $prior[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
try {
    $env:NR_R01_REPO = $repo
    $env:NR_R01_OUTPUT = $output
    $env:NR_R01_VSDEV = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
    # AddonInit's normal logs/recovery markers must stay in this isolated batch.
    $env:LOCALAPPDATA = $output
    $body = @'
@echo off
call "%NR_R01_VSDEV%" -no_logo -arch=x64 >nul
if errorlevel 1 exit /b 1
cd /d "%NR_R01_OUTPUT%"
set "FEEDER=%NR_R01_REPO%\carrier\external\DLSS5-Feeder"
cl /nologo /std:c++20 /EHsc /O2 /MD /utf-8 /LD ^
 /I"%FEEDER%\external\ngx" /I"%FEEDER%\external\reshade\include" ^
 /I"%FEEDER%\external\imgui" /I"%FEEDER%\external\vulkan" /I"%FEEDER%\external\minhook\include" ^
 /I"%NR_R01_REPO%\carrier\addon\include" ^
 "%NR_R01_REPO%\tests\lifecycle_fixture.cpp" "%NR_R01_REPO%\carrier\addon\src\nvof-motion-provider.cpp" ^
 "%FEEDER%\external\minhook\src\buffer.c" "%FEEDER%\external\minhook\src\hook.c" ^
 "%FEEDER%\external\minhook\src\trampoline.c" "%FEEDER%\external\minhook\src\hde\hde64.c" ^
 /link /OUT:nr-lifecycle-fixture.dll kernel32.lib user32.lib d3d9.lib d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib dcomp.lib windowscodecs.lib ole32.lib uuid.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MD /utf-8 "%NR_R01_REPO%\tests\lifecycle_host.cpp" /Fe:lifecycle-host.exe
if errorlevel 1 exit /b 1
exit /b %ERRORLEVEL%
'@
    $cmd = Join-Path $output 'run.cmd'
    [IO.File]::WriteAllText($cmd,$body,[Text.Encoding]::ASCII)
    & $env:ComSpec /d /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "Shutdown regression failed; evidence: $output" }
    $stdout = Join-Path $output 'result.log'
    $stderr = Join-Path $output 'errors.log'
    $hostExe = Join-Path $output 'lifecycle-host.exe'
    $fixture = Join-Path $output 'nr-lifecycle-fixture.dll'
    $testProcess = Start-Process -FilePath $hostExe -ArgumentList ('"' + $fixture + '"') `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $null = $testProcess.Handle # Cache the handle before exit (Windows PowerShell ExitCode reporting).
    if (-not $testProcess.WaitForExit(45000)) {
        Stop-Process -Id $testProcess.Id -Force
        throw "Shutdown regression exceeded 45 seconds; evidence: $output"
    }
    $testProcess.WaitForExit()
    [IO.File]::ReadAllText($stdout, [Text.UTF8Encoding]::new($false)) | Write-Output
    $failure = [IO.File]::ReadAllText($stderr, [Text.UTF8Encoding]::new($false))
    if ($failure) { Write-Output $failure }
    if ($testProcess.ExitCode -ne 0) { throw "Shutdown regression failed; evidence: $output" }
    Write-Output "Shutdown regression passed; evidence: $output"
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name,$prior[$name],'Process') }
}
