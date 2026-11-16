# Build-ControlCenter.ps1 — 编译 WOW-335-NR 原生控制中心（C++17 / Win32 / Direct2D）。
#
# 约定（与父级 AGENTS.md 一致）：
#   * 构建只写入 D:\WOW-335-CN\_scratch，不覆盖现役程序；
#   * 输出目录同时带上发行清单与暂存模板，便于程序从自身目录定位资源；
#   * /MT 静态链接 C/C++ 运行库，目标最低 Windows 10 x64，不依赖 .NET、VC++ 可再发行包等外部运行时；
#   * 产物是候选：build.json 记录源码哈希与状态，不等同于干净系统验收。
param([string]$OutputDirectory)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$scratch = [IO.Path]::GetFullPath((Join-Path $project '_scratch'))
$repo = [IO.Path]::GetFullPath($PSScriptRoot)
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $scratch ('dlss5-aio/native-ui-candidate-' + [guid]::NewGuid().ToString('N'))
}
$output = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd([IO.Path]::DirectorySeparatorChar)
$scratchPrefix = $scratch.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if (-not $output.StartsWith($scratchPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw '构建输出必须位于 _scratch。'
}
if (Test-Path -LiteralPath $output) { throw '构建输出目录已存在。' }
if (-not (Test-Path -LiteralPath (Join-Path $repo 'native/control_center/app/cc_main.cpp'))) {
    throw '未找到原生控制中心源码。'
}
$current = $output
while ($current) {
    $item = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
    if ($null -ne $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "构建路径含链接：$current"
    }
    $parent = [IO.Path]::GetDirectoryName($current.TrimEnd([IO.Path]::DirectorySeparatorChar))
    if (-not $parent -or $parent -eq $current) { break }
    $current = $parent
}

$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not [IO.File]::Exists($vswhere)) { throw '未找到 Visual Studio 安装定位工具。' }
$install = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or @($install).Count -ne 1 -or -not $install) { throw '未找到 C++ 编译工具。' }
$vsdev = Join-Path $install 'Common7/Tools/VsDevCmd.bat'
if (-not [IO.File]::Exists($vsdev)) { throw '未找到 C++ 编译环境脚本。' }

[IO.Directory]::CreateDirectory($output) | Out-Null
[IO.Directory]::CreateDirectory((Join-Path $output 'build')) | Out-Null
$appDir = Join-Path $repo 'native/control_center/app'
$coreDir = Join-Path $repo 'native/control_center'
$exe = Join-Path $output 'WOW-335-NR-ControlCenter.exe'
$cmd = Join-Path $output 'build.cmd'
$body = @"
@echo off
cd /d "%DLSS5_TASK_OUT%"
call "%DLSS5_TASK_VSDEV%" -no_logo -arch=x64 >nul
if errorlevel 1 exit /b 1
pushd "%DLSS5_TASK_APP%"
rc /nologo /fo "%DLSS5_TASK_OUT%\build\control_center.res" control_center.rc > "%DLSS5_TASK_OUT%\build.log" 2>&1
set "RC_EXIT=%ERRORLEVEL%"
popd
if not "%RC_EXIT%"=="0" exit /b %RC_EXIT%
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_WIN32_WINNT=0x0A00 ^
  /I"%DLSS5_TASK_APP%" /I"%DLSS5_TASK_CORE%" ^
  "%DLSS5_TASK_APP%\cc_main.cpp" "%DLSS5_TASK_APP%\cc_pages.cpp" ^
  "%DLSS5_TASK_OUT%\build\control_center.res" ^
  /Fobuild\ /Febuild\WOW-335-NR-ControlCenter.exe ^
  /link /SUBSYSTEM:WINDOWS /OUT:"%DLSS5_TASK_EXE%" user32.lib gdi32.lib d2d1.lib dwrite.lib bcrypt.lib dxgi.lib ^
  ole32.lib oleaut32.lib shell32.lib uuid.lib >> "%DLSS5_TASK_OUT%\build.log" 2>&1
exit /b %ERRORLEVEL%
"@
[IO.File]::WriteAllText($cmd, $body, [Text.Encoding]::ASCII)

$names = @('DLSS5_TASK_VSDEV', 'DLSS5_TASK_APP', 'DLSS5_TASK_CORE', 'DLSS5_TASK_EXE', 'DLSS5_TASK_OUT')
$old = @{}
foreach ($name in $names) { $old[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    [Environment]::SetEnvironmentVariable('DLSS5_TASK_VSDEV', $vsdev, 'Process')
    [Environment]::SetEnvironmentVariable('DLSS5_TASK_APP', $appDir, 'Process')
    [Environment]::SetEnvironmentVariable('DLSS5_TASK_CORE', $coreDir, 'Process')
    [Environment]::SetEnvironmentVariable('DLSS5_TASK_EXE', $exe, 'Process')
    [Environment]::SetEnvironmentVariable('DLSS5_TASK_OUT', $output, 'Process')
    & $env:ComSpec /d /c $cmd
    if ($LASTEXITCODE -ne 0 -or -not [IO.File]::Exists($exe)) {
        $logPath = Join-Path $output 'build.log'
        $log = if (Test-Path -LiteralPath $logPath) { Get-Content -LiteralPath $logPath -Raw } else { '' }
        throw ('原生控制中心编译失败。' + $log)
    }
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $old[$name], 'Process') }
}

# 发行清单与模板：程序只从自身目录读取（产物自带，不依赖维护仓库相对路径）。
foreach ($name in @('install-manifest.json', 'payload-lock.json')) {
    $source = Join-Path $repo $name
    if (-not (Test-Path -LiteralPath $source)) { throw "缺少发行文件：$name" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $output $name)
}
$templateSource = Join-Path $repo 'templates'
if (-not (Test-Path -LiteralPath $templateSource)) { throw "缺少模板目录：templates" }
Copy-Item -LiteralPath $templateSource -Destination (Join-Path $output 'templates') -Recurse
# 锁文件与面板/助手：便携包自带身份记录；工作区部署位存在运行组件时一并打包到 bin\。
foreach ($name in @('watcher-lock.json', 'panel-lock.json')) {
    $source = Join-Path $repo $name
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $output $name)
    }
}
$binDir = Join-Path $output 'bin'
[IO.Directory]::CreateDirectory($binDir) | Out-Null
$toolsBin = Join-Path $project 'tools/dlss5/bin'
$bundled = [ordered]@{}
foreach ($name in @('dlss5-panel.exe', 'window_watch.exe')) {
    $source = Join-Path $toolsBin $name
    if (-not (Test-Path -LiteralPath $source)) { continue }
    $target = Join-Path $binDir $name
    Copy-Item -LiteralPath $source -Destination $target
    $bundled[$name] = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
}

# 运行依赖审核：/MT 静态运行库构建；dumpbin /dependents 确认不引入 VC++ 运行库。
$auditCmd = Join-Path $output 'audit.cmd'
$auditBody = @"
@echo off
call "%DLSS5_TASK_VSDEV%" -no_logo -arch=x64 >nul
dumpbin /nologo /dependents "%DLSS5_TASK_EXE%" > "%DLSS5_TASK_OUT%\dependencies.txt"
exit /b %ERRORLEVEL%
"@
[IO.File]::WriteAllText($auditCmd, $auditBody, [Text.Encoding]::ASCII)
$auditOld = @{}
foreach ($name in @('DLSS5_TASK_VSDEV', 'DLSS5_TASK_EXE', 'DLSS5_TASK_OUT')) {
    $auditOld[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    [Environment]::SetEnvironmentVariable('DLSS5_TASK_VSDEV', $vsdev, 'Process')
    [Environment]::SetEnvironmentVariable('DLSS5_TASK_EXE', $exe, 'Process')
    [Environment]::SetEnvironmentVariable('DLSS5_TASK_OUT', $output, 'Process')
    & $env:ComSpec /d /c $auditCmd
    if ($LASTEXITCODE -ne 0) { throw '运行依赖审核失败（dumpbin /dependents）。' }
} finally {
    foreach ($name in $auditOld.Keys) {
        [Environment]::SetEnvironmentVariable($name, $auditOld[$name], 'Process')
    }
}
$dependencyText = [IO.File]::ReadAllText((Join-Path $output 'dependencies.txt'))
$imports = @([regex]::Matches($dependencyText, '(?m)^\s+([A-Za-z0-9_.\-]+\.dll)\s*$') |
             ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() } | Sort-Object -Unique)
$runtimeImports = @($imports | Where-Object { $_ -match '^(vcruntime|msvcp|ucrtbase|concrt|vcomp|api-ms-win-crt)' })
if ($runtimeImports.Count -gt 0) {
    throw ('便携要求静态运行库，但产物依赖：' + ($runtimeImports -join ', '))
}
$panelLockMatch = $null
$watchLockMatch = $null
$panelLockPath = Join-Path $output 'panel-lock.json'
if ($bundled.Contains('dlss5-panel.exe') -and (Test-Path -LiteralPath $panelLockPath)) {
    $panelLockText = Get-Content -LiteralPath $panelLockPath -Raw | ConvertFrom-Json
    $panelLockMatch = ([string]$panelLockText.files.'dlss5-panel.exe' -eq $bundled['dlss5-panel.exe'])
}
$watchLockPath = Join-Path $output 'watcher-lock.json'
if ($bundled.Contains('window_watch.exe') -and (Test-Path -LiteralPath $watchLockPath)) {
    $watchLockText = Get-Content -LiteralPath $watchLockPath -Raw | ConvertFrom-Json
    $watchLockMatch = ([string]$watchLockText.files.'window_watch.exe' -eq $bundled['window_watch.exe'])
}

$sources = [ordered]@{}
foreach ($file in (Get-ChildItem -LiteralPath $appDir -File | Where-Object { $_.Extension -in '.cpp', '.hpp', '.h', '.rc', '.ico' } |
                   Sort-Object Name)) {
    $sources[$file.Name] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
foreach ($file in (Get-ChildItem -LiteralPath $coreDir -File | Where-Object { $_.Extension -eq '.hpp' } |
                   Sort-Object Name)) {
    $sources[('control_center/' + $file.Name)] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
$layout = @('WOW-335-NR-ControlCenter.exe', 'install-manifest.json', 'payload-lock.json', 'templates')
foreach ($name in @('watcher-lock.json', 'panel-lock.json')) {
    if (Test-Path -LiteralPath (Join-Path $output $name)) { $layout += $name }
}
if ($bundled.Count -gt 0) { $layout += 'bin' }
$record = [ordered]@{
    schema = 1
    state = 'compiled_candidate_native_ui'
    note = '编译通过的原生控制中心候选：安装/修复/卸载写事务、Config.wtf 写入、ReShade 参数与方案、运行时遥测、GPU 统计、面板/日志、诊断与清单导出、恢复安装前文件、AIO 管线开关。候选仍需干净系统与真实游戏验收。'
    compiler = $install
    sources = $sources
    executable_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    portable = [ordered]@{
        layout = $layout
        bundled_binaries = $bundled
        panel_lock_match = $panelLockMatch
        watch_lock_match = $watchLockMatch
        static_runtime = $true
        dependencies = $imports
        dependencies_file = 'dependencies.txt'
    }
    generated_utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
}
[IO.File]::WriteAllText((Join-Path $output 'build.json'), (($record | ConvertTo-Json -Depth 5) + "`n"),
                        [Text.UTF8Encoding]::new($false))
[pscustomobject]@{
    Output = $exe
    Directory = $output
    Record = (Join-Path $output 'build.json')
    Sha256 = $record.executable_sha256
}
