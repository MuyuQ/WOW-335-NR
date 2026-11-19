# Build-ControlCenter.ps1 — 编译 WOW-335-NR 原生控制中心（C++17 / Win32 / Direct2D）。
#
# 约定（与父级 AGENTS.md 一致）：
#   * 构建只写入 D:\WOW-335-CN\_scratch，不覆盖现役程序，也不写 tools/dlss5/bin 部署目录；
#   * 输出目录同时带上发行清单与暂存模板，便于程序从自身目录定位资源；
#   * /MT 静态链接 C/C++ 运行库，目标最低 Windows 10 x64，不依赖 .NET、VC++ 可再发行包等外部运行时；
#   * 产物是候选：build.json 记录源码哈希与状态，不等同于干净系统验收；
#   * 面板必须由 -PanelCandidate（或 -PanelCandidateDirectory）显式给出，并核对 panel-lock.json；
#     部署位可以仍是上一轮旧面板，构建不从那里取面板，部署时再单独同步。
param(
    [string]$OutputDirectory,
    [string]$PanelCandidate,
    [string]$PanelCandidateDirectory,
    [switch]$AllowUnverified
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($PanelCandidate -and $PanelCandidateDirectory) {
    throw '不能同时指定 -PanelCandidate 与 -PanelCandidateDirectory。'
}

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

# R7（运行期与构建一致性）：默认严格校验 panel-lock.json / watcher-lock.json 与打包组件的实际哈希。
# 面板只从显式候选取件：-PanelCandidate 指定 exe 路径，-PanelCandidateDirectory 指定包含
# dlss5-panel.exe 的目录。候选必须存在，SHA-256 必须等于 panel-lock.json 的 dlss5-panel.exe 记录；
# native/panel.cpp 的实际哈希也须与锁一致。任一不符时在创建输出目录前终止，避免把旧面板或
# 未校验候选标成 compiled_candidate_native_ui。窗口助手仍从 tools/dlss5/bin 取件并核对
# watcher-lock.json。显式 -AllowUnverified 时放行，但 build.json 会标注 unverified 并记录逐项
# 原因；它只放宽终止条件，不改变取件来源，不用于生成可验收候选。
$toolsBin = Join-Path $project 'tools/dlss5/bin'

function Get-Sha256Hex {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Read-LockDocument {
    param([Parameter(Mandatory = $true)][string]$Path)
    $document = [pscustomobject]@{ Present = $false; Data = $null; Error = $null }
    if (-not (Test-Path -LiteralPath $Path)) {
        $document.Error = '文件不存在'
        return $document
    }
    $document.Present = $true
    try {
        $document.Data = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    } catch {
        $document.Error = 'JSON 无法解析：' + $_.Exception.Message
    }
    return $document
}

function Get-LockFieldValue {
    param($Document, [Parameter(Mandatory = $true)][string]$Field)
    if ($null -eq $Document) { return $null }
    $filesProperty = $Document.PSObject.Properties['files']
    if ($null -eq $filesProperty -or $null -eq $filesProperty.Value) { return $null }
    $property = $filesProperty.Value.PSObject.Properties[$Field]
    if ($null -eq $property) { return $null }
    return [string]$property.Value
}

function Test-LockDocument {
    param(
        [Parameter(Mandatory = $true)][string]$FileName,
        [Parameter(Mandatory = $true)]$Document,
        [Parameter(Mandatory = $true)][string[]]$Fields
    )
    if (-not $Document.Present) { return @("缺少 $FileName") }
    if ($null -eq $Document.Data) { return @("$FileName 无法解析：$($Document.Error)") }
    $errors = @()
    $schemaProperty = $Document.Data.PSObject.Properties['schema']
    if ($null -eq $schemaProperty -or [int]$schemaProperty.Value -ne 1) {
        $errors += "$FileName 的 schema 不是 1"
    }
    $filesProperty = $Document.Data.PSObject.Properties['files']
    if ($null -eq $filesProperty -or $null -eq $filesProperty.Value) {
        $errors += "$FileName 缺少 files 对象"
        return $errors
    }
    foreach ($field in $Fields) {
        $property = $filesProperty.Value.PSObject.Properties[$field]
        $value = if ($null -ne $property) { [string]$property.Value } else { $null }
        if (-not $value -or $value -notmatch '^[0-9a-f]{64}$') {
            $errors += "$FileName 的 $field 不是 64 位十六进制哈希"
        }
    }
    return $errors
}

$panelLockDocument = Read-LockDocument -Path (Join-Path $repo 'panel-lock.json')
$watchLockDocument = Read-LockDocument -Path (Join-Path $repo 'watcher-lock.json')
$panelLockHash = Get-LockFieldValue -Document $panelLockDocument.Data -Field 'dlss5-panel.exe'
$panelCppLockHash = Get-LockFieldValue -Document $panelLockDocument.Data -Field 'panel.cpp'
$watchLockHash = Get-LockFieldValue -Document $watchLockDocument.Data -Field 'window_watch.exe'

$panelErrors = @(Test-LockDocument -FileName 'panel-lock.json' -Document $panelLockDocument -Fields @('dlss5-panel.exe', 'panel.cpp'))
$watchErrors = @(Test-LockDocument -FileName 'watcher-lock.json' -Document $watchLockDocument -Fields @('window_watch.exe', 'window_watch.cpp', 'window_layer.hpp'))

# 候选解析：-PanelCandidate 优先；-PanelCandidateDirectory 是便于测试/固定目录的简写形式。
$candidatePath = $null
if ($PanelCandidate) {
    $candidatePath = [IO.Path]::GetFullPath($PanelCandidate)
} elseif ($PanelCandidateDirectory) {
    $candidatePath = [IO.Path]::GetFullPath((Join-Path $PanelCandidateDirectory 'dlss5-panel.exe'))
}
$panelArtifact = [ordered]@{
    source_path = $candidatePath
    source_sha256 = $null
    packaged_sha256 = $null
    packaged_matches_source = $null
    lock_sha256 = $panelLockHash
    matches_lock = $false
    deployed_panel_sha256 = $null
    deployed_matches_lock = $null
}
$panelCppHash = $null
$panelCppMatch = $null
if ($null -eq $candidatePath) {
    $message = '必须用 -PanelCandidate（或 -PanelCandidateDirectory）指定已验证的面板候选；构建不再从 tools/dlss5/bin 取面板。'
    if (-not $AllowUnverified) { throw $message }
    $panelErrors += '未提供面板候选（本次 -AllowUnverified，不打包面板）：' + $message
} elseif (-not [IO.File]::Exists($candidatePath)) {
    $panelErrors += "面板候选不存在：$candidatePath"
} elseif ($panelLockHash -notmatch '^[0-9a-f]{64}$') {
    $panelErrors += 'panel-lock.json 未提供有效的 dlss5-panel.exe 哈希，无法核对候选。'
} else {
    $sourceHash = Get-Sha256Hex -Path $candidatePath
    $panelArtifact['source_sha256'] = $sourceHash
    if ($sourceHash -eq $panelLockHash) {
        $panelArtifact['matches_lock'] = $true
    } else {
        $panelErrors += "面板候选 SHA-256（$sourceHash）与 panel-lock.json 记录（$panelLockHash）不符"
    }
}

# 面板源码哈希同样以锁为准：源码变了却没重编候选/没更新锁时在这里暴露。
$panelCppPath = Join-Path $repo 'native/panel.cpp'
if (-not [IO.File]::Exists($panelCppPath)) {
    $panelErrors += '缺少面板源码：native/panel.cpp'
} else {
    $panelCppHash = Get-Sha256Hex -Path $panelCppPath
    if ($panelCppLockHash -notmatch '^[0-9a-f]{64}$') {
        $panelErrors += 'panel-lock.json 未提供有效的 panel.cpp 哈希，无法核对面板源码。'
    } elseif ($panelCppHash -eq $panelCppLockHash) {
        $panelCppMatch = $true
    } else {
        $panelCppMatch = $false
        $panelErrors += "native/panel.cpp 实际 SHA-256（$panelCppHash）与 panel-lock.json 记录（$panelCppLockHash）不符"
    }
}
$panelArtifact['panel_cpp'] = [ordered]@{
    path = 'native/panel.cpp'
    sha256 = $panelCppHash
    lock_sha256 = $panelCppLockHash
    matches_lock = $panelCppMatch
}

# 部署位仅作信息记录：它可能仍是上一轮旧件，不参与取件，也不阻断构建。
$deployedPanelPath = Join-Path $toolsBin 'dlss5-panel.exe'
if ([IO.File]::Exists($deployedPanelPath)) {
    $deployedPanelHash = Get-Sha256Hex -Path $deployedPanelPath
    $panelArtifact['deployed_panel_sha256'] = $deployedPanelHash
    if ($panelLockHash -match '^[0-9a-f]{64}$') {
        $panelArtifact['deployed_matches_lock'] = ($deployedPanelHash -eq $panelLockHash)
    }
}

# 窗口助手仍按 watcher-lock.json 核对 tools/dlss5/bin 中的部署件。
$watchSourcePath = Join-Path $toolsBin 'window_watch.exe'
$watchActualHash = if ([IO.File]::Exists($watchSourcePath)) { Get-Sha256Hex -Path $watchSourcePath } else { $null }
if ($null -eq $watchActualHash) {
    $watchErrors += '缺少打包组件：tools/dlss5/bin/window_watch.exe'
} elseif ($watchLockHash -match '^[0-9a-f]{64}$' -and $watchActualHash -ne $watchLockHash) {
    $watchErrors += "watcher-lock.json 的 window_watch.exe 与 tools/dlss5/bin/window_watch.exe 实际哈希不符"
}
$watchLockMatch = ($null -ne $watchActualHash -and $watchLockHash -match '^[0-9a-f]{64}$' -and $watchActualHash -eq $watchLockHash)
$panelLockMatch = [bool]$panelArtifact.matches_lock
$lockResults = [ordered]@{
    'panel-lock.json' = [ordered]@{
        file = 'panel-lock.json'
        present = $panelLockDocument.Present
        verified = ($panelErrors.Count -eq 0)
        match = $panelLockMatch
        exe_source = 'panel_candidate'
        exe_record_sha256 = $panelLockHash
        candidate_sha256 = $panelArtifact.source_sha256
        error = ($panelErrors -join '；')
    }
    'watcher-lock.json' = [ordered]@{
        file = 'watcher-lock.json'
        present = $watchLockDocument.Present
        verified = ($watchErrors.Count -eq 0)
        match = $watchLockMatch
        exe_source = 'tools/dlss5/bin'
        exe_record_sha256 = $watchLockHash
        candidate_sha256 = $watchActualHash
        error = ($watchErrors -join '；')
    }
}
$unverifiedReasons = @($panelErrors) + @($watchErrors)
if ($unverifiedReasons.Count -gt 0 -and -not $AllowUnverified) {
    throw ('运行期身份校验失败：' + ($unverifiedReasons -join '；') + '。如确需生成未校验候选，请显式使用 -AllowUnverified。')
}

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

# Built-in schemes are required resources, verified in the final portable layout.
$schemeSource = Join-Path $repo 'schemes'
$schemeTarget = Join-Path $output 'schemes'
[IO.Directory]::CreateDirectory($schemeTarget) | Out-Null
$schemeHashes = [ordered]@{}
foreach ($name in @('原画', '轻量', '均衡', '浓郁')) {
    $source = Join-Path $schemeSource ($name + '.json')
    $target = Join-Path $schemeTarget ($name + '.json')
    $scheme = Get-Content -LiteralPath $source -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($scheme.schema -ne 1 -or $scheme.name -ne $name -or $null -eq $scheme.values) {
        throw "内置方案无效：$name"
    }
    Copy-Item -LiteralPath $source -Destination $target
    $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hash) {
        throw "内置方案复制校验失败：$name"
    }
    $schemeHashes[('schemes/' + $name + '.json')] = $hash
}

# 锁文件与面板/助手：便携包自带身份记录。面板从本次候选取件，复制后回读包内完整 SHA-256；
# 窗口助手仍从工作区部署位取件（已按 watcher-lock.json 校验）。本脚本不写 tools/dlss5/bin。
foreach ($name in @('watcher-lock.json', 'panel-lock.json')) {
    $source = Join-Path $repo $name
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $output $name)
    }
}
$binDir = Join-Path $output 'bin'
[IO.Directory]::CreateDirectory($binDir) | Out-Null
$bundled = [ordered]@{}
if ($null -ne $panelArtifact.source_path -and [IO.File]::Exists([string]$panelArtifact.source_path)) {
    $panelTarget = Join-Path $binDir 'dlss5-panel.exe'
    Copy-Item -LiteralPath $panelArtifact.source_path -Destination $panelTarget
    $packagedHash = Get-Sha256Hex -Path $panelTarget
    $panelArtifact['packaged_sha256'] = $packagedHash
    $panelArtifact['packaged_matches_source'] = ($packagedHash -eq $panelArtifact['source_sha256'])
    $copyErrors = @()
    if (-not $panelArtifact.packaged_matches_source) {
        $copyErrors += "包内 bin/dlss5-panel.exe 哈希（$packagedHash）与候选源（$($panelArtifact.source_sha256)）不一致"
    }
    if ($panelLockHash -match '^[0-9a-f]{64}$' -and $packagedHash -ne $panelLockHash) {
        $copyErrors += "包内 bin/dlss5-panel.exe 哈希（$packagedHash）与 panel-lock.json 记录（$panelLockHash）不一致"
    }
    if ($copyErrors.Count -gt 0) {
        if (-not $AllowUnverified) { throw ('面板复制校验失败：' + ($copyErrors -join '；')) }
        $unverifiedReasons += $copyErrors
    }
    $bundled['dlss5-panel.exe'] = $packagedHash
}
if ([IO.File]::Exists($watchSourcePath)) {
    $watchTarget = Join-Path $binDir 'window_watch.exe'
    Copy-Item -LiteralPath $watchSourcePath -Destination $watchTarget
    $bundled['window_watch.exe'] = Get-Sha256Hex -Path $watchTarget
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
$panelLockMatch = [bool]$lockResults['panel-lock.json'].match
$watchLockMatch = [bool]$lockResults['watcher-lock.json'].match

$sources = [ordered]@{}
foreach ($key in $schemeHashes.Keys) { $sources[$key] = $schemeHashes[$key] }
foreach ($file in (Get-ChildItem -LiteralPath $appDir -File | Where-Object { $_.Extension -in '.cpp', '.hpp', '.h', '.rc', '.ico' } |
                   Sort-Object Name)) {
    $sources[$file.Name] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
foreach ($file in (Get-ChildItem -LiteralPath $coreDir -File | Where-Object { $_.Extension -eq '.hpp' } |
                   Sort-Object Name)) {
    $sources[('control_center/' + $file.Name)] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
$layout = @('WOW-335-NR-ControlCenter.exe', 'install-manifest.json', 'payload-lock.json', 'templates', 'schemes')
foreach ($name in @('watcher-lock.json', 'panel-lock.json')) {
    if (Test-Path -LiteralPath (Join-Path $output $name)) { $layout += $name }
}
if ($bundled.Count -gt 0) { $layout += 'bin' }
$verified = ($unverifiedReasons.Count -eq 0)
$recordState = if ($verified) { 'compiled_candidate_native_ui' } else { 'compiled_candidate_native_ui_unverified' }
$recordNote = '编译通过的原生控制中心候选：安装/修复/卸载写事务、Config.wtf 写入、ReShade 参数与方案、运行时遥测、GPU 统计、面板/日志、诊断与清单导出、恢复安装前文件、AIO 管线开关。面板来自显式候选并对包内 EXE 回读哈希，不从 tools/dlss5/bin 取部署件。候选仍需干净系统与真实游戏验收。'
if (-not $verified) {
    $recordNote += ' 未校验（-AllowUnverified）：' + ($unverifiedReasons -join '；') + '。'
}
$record = [ordered]@{
    schema = 1
    state = $recordState
    unverified = (-not $verified)
    note = $recordNote
    compiler = $install
    panel_candidate = $panelArtifact
    sources = $sources
    executable_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    portable = [ordered]@{
        scheme_sha256 = $schemeHashes
        layout = $layout
        bundled_binaries = $bundled
        panel_lock_match = $panelLockMatch
        watch_lock_match = $watchLockMatch
        panel_lock = $lockResults['panel-lock.json']
        watch_lock = $lockResults['watcher-lock.json']
        lock_verification = [ordered]@{
            verified = $verified
            allow_unverified = [bool]$AllowUnverified
            errors = @($unverifiedReasons)
        }
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
    PanelCandidate = $panelArtifact.source_path
    PanelSha256 = $panelArtifact.packaged_sha256
}
