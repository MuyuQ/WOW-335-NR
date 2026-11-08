param([string]$OutputDirectory)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$scratch = [IO.Path]::GetFullPath((Join-Path $project '_scratch'))
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $scratch ('dlss5-aio/window-build-' + [guid]::NewGuid().ToString('N'))
}
$output = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd([IO.Path]::DirectorySeparatorChar)
if (-not $output.StartsWith($scratch.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar,
                           [StringComparison]::OrdinalIgnoreCase)) { throw '构建输出必须位于 _scratch。' }
if (Test-Path -LiteralPath $output) { throw '构建输出目录已存在。' }
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
$cpp = Join-Path $PSScriptRoot 'native/window_watch.cpp'
$header = Join-Path $PSScriptRoot 'native/window_layer.hpp'
$exe = Join-Path $output 'window_watch.exe'
$obj = Join-Path $output 'window_watch.obj'
$cmd = Join-Path $output 'build.cmd'
$body = @'
@echo off
call "%DLSS5_TASK_VSDEV%" -no_logo -arch=x64 >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /O2 /MT /DUNICODE /D_UNICODE /utf-8 "%DLSS5_TASK_CPP%" /Fo:"%DLSS5_TASK_OBJ%" /Fe:"%DLSS5_TASK_EXE%" /link user32.lib advapi32.lib
exit /b %ERRORLEVEL%
'@
[IO.File]::WriteAllText($cmd, $body, [Text.Encoding]::ASCII)
$names = @('DLSS5_TASK_VSDEV', 'DLSS5_TASK_CPP', 'DLSS5_TASK_OBJ', 'DLSS5_TASK_EXE')
$old = @{}
foreach ($name in $names) { $old[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    [Environment]::SetEnvironmentVariable('DLSS5_TASK_VSDEV', $vsdev, 'Process')
    [Environment]::SetEnvironmentVariable('DLSS5_TASK_CPP', $cpp, 'Process')
    [Environment]::SetEnvironmentVariable('DLSS5_TASK_OBJ', $obj, 'Process')
    [Environment]::SetEnvironmentVariable('DLSS5_TASK_EXE', $exe, 'Process')
    & $env:ComSpec /d /c $cmd
    if ($LASTEXITCODE -ne 0 -or -not [IO.File]::Exists($exe)) { throw '窗口助手编译失败。' }
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $old[$name], 'Process') }
}
$record = [ordered]@{ schema = 1; state = 'compiled_candidate_not_game_validated'; compiler = $install;
    source_sha256 = (Get-FileHash -LiteralPath $cpp -Algorithm SHA256).Hash.ToLowerInvariant();
    header_sha256 = (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash.ToLowerInvariant();
    executable_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant() }
[IO.File]::WriteAllText((Join-Path $output 'build.json'), (($record | ConvertTo-Json -Depth 4) + "`n"),
                        [Text.UTF8Encoding]::new($false))
[pscustomobject]@{ Output = $exe; Record = (Join-Path $output 'build.json'); Sha256 = $record.executable_sha256 }
