param([string]$VisualStudioPath = 'C:\Program Files\Microsoft Visual Studio\2022\Community')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workspace = [IO.Path]::GetFullPath((Join-Path $repo '../..'))
$scratch = [IO.Path]::GetFullPath((Join-Path $workspace '_scratch'))
$output = Join-Path $scratch ('dlss5-aio/control-center-test-' + [guid]::NewGuid().ToString('N'))
$current = $output
while ($current) {
    $item = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
    if ($null -ne $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Build path contains a reparse point: $current" }
    $parent = [IO.Path]::GetDirectoryName($current.TrimEnd([IO.Path]::DirectorySeparatorChar))
    if (-not $parent -or $parent -eq $current) { break }
    $current = $parent
}
$vsdev = Join-Path $VisualStudioPath 'Common7/Tools/VsDevCmd.bat'
if (-not [IO.File]::Exists($vsdev)) { throw "VS 2022 build tools not found: $vsdev" }
if (Test-Path -LiteralPath $output) { throw 'Test output already exists.' }
[IO.Directory]::CreateDirectory($output) | Out-Null
$build = @'
@echo off
cd /d "%CC_TEST_OUTPUT%"
call "%CC_TEST_VSDEV%" -no_logo -arch=x64
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_paths.cpp" /Fe:paths.exe
if errorlevel 1 exit /b 1
paths.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_file_identity.cpp" /Fe:identity.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
identity.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
mkdir "%CC_TEST_OUTPUT%\real"
if errorlevel 1 exit /b 1
mklink /J "%CC_TEST_OUTPUT%\link" "%CC_TEST_OUTPUT%\real" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_guarded_path.cpp" /Fe:guarded.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
guarded.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_client.cpp" /Fe:client.exe
if errorlevel 1 exit /b 1
client.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_client_lock.cpp" /Fe:client-lock.exe
if errorlevel 1 exit /b 1
client-lock.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_copy.cpp" /Fe:copy.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
copy.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_rename.cpp" /Fe:rename.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
rename.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_write_new.cpp" /Fe:write-new.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
write-new.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_delete.cpp" /Fe:delete.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
delete.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_journal.cpp" /Fe:journal.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
journal.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_reconcile.cpp" /Fe:reconcile.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
reconcile.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_install_member.cpp" /Fe:install-member.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
install-member.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_final_records.cpp" /Fe:final-records.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
final-records.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_record_recovery.cpp" /Fe:record-recovery.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
record-recovery.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_commit.cpp" /Fe:commit.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
commit.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_install_job.cpp" /Fe:install-job.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
install-job.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_preserved_config.cpp" /Fe:preserved-config.exe /link bcrypt.lib ole32.lib
if errorlevel 1 exit /b 1
preserved-config.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_repair_preview.cpp" /Fe:repair-preview.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
repair-preview.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_repair_member.cpp" /Fe:repair-member.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
repair-member.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_repair_job.cpp" /Fe:repair-job.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
repair-job.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_repair_multi.cpp" /Fe:repair-multi.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
repair-multi.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_uninstall_preview.cpp" /Fe:uninstall-preview.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
uninstall-preview.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_uninstall_journal.cpp" /Fe:uninstall-journal.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
uninstall-journal.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_uninstall_member.cpp" /Fe:uninstall-member.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
uninstall-member.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_uninstall_finalize.cpp" /Fe:uninstall-finalize.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
uninstall-finalize.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_config_wtf.cpp" /Fe:config-wtf.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
config-wtf.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_ds5_settings.cpp" /Fe:ds5-settings.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
ds5-settings.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_ds5_schemes.cpp" /Fe:ds5-schemes.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
ds5-schemes.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_telemetry.cpp" /Fe:telemetry.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
telemetry.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_pipeline_switch.cpp" /Fe:pipeline-switch.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
pipeline-switch.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_window_watch.cpp" /Fe:window-watch.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
window-watch.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_restore_operation.cpp" /Fe:restore-operation.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
restore-operation.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_manifest_list.cpp" /Fe:manifest-list.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
manifest-list.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_diagnostics.cpp" /Fe:diagnostics.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
diagnostics.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_panel_process.cpp" /Fe:panel-process.exe /link bcrypt.lib user32.lib
if errorlevel 1 exit /b 1
panel-process.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_production_compile.cpp" /Fe:production-compile.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
production-compile.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_crash.cpp" /Fe:crash.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
crash.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_preview.cpp" /Fe:preview.exe /link bcrypt.lib
if errorlevel 1 exit /b 1
preview.exe "%CC_TEST_OUTPUT%"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_json.cpp" /Fe:json.exe
if errorlevel 1 exit /b 1
json.exe "%CC_TEST_REPO%\install-manifest.json" "%CC_TEST_REPO%\payload-lock.json"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_catalog.cpp" /Fe:catalog.exe
if errorlevel 1 exit /b 1
catalog.exe "%CC_TEST_REPO%\install-manifest.json" "%CC_TEST_REPO%\payload-lock.json"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_client_record.cpp" /Fe:client-record.exe
if errorlevel 1 exit /b 1
client-record.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /utf-8 "%CC_TEST_REPO%\tests\control_center_operation_record.cpp" /Fe:operation-record.exe
if errorlevel 1 exit /b 1
operation-record.exe
exit /b %ERRORLEVEL%
'@
$cmd = Join-Path $output 'run.cmd'
[IO.File]::WriteAllText($cmd, $build, [Text.Encoding]::ASCII)
$names = @('CC_TEST_OUTPUT', 'CC_TEST_VSDEV', 'CC_TEST_REPO')
$old = @{}
foreach ($name in $names) { $old[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    [Environment]::SetEnvironmentVariable('CC_TEST_OUTPUT', $output, 'Process')
    [Environment]::SetEnvironmentVariable('CC_TEST_VSDEV', $vsdev, 'Process')
    [Environment]::SetEnvironmentVariable('CC_TEST_REPO', $repo, 'Process')
    & $env:ComSpec /d /c $cmd
    if ($LASTEXITCODE -ne 0) { throw "Control center tests failed: $LASTEXITCODE" }

    # 图层按钮必须把请求发给“所选客户端”的载具：用两个假载具进程验证宿主选择与消息路由。
    $panelProcess = Join-Path $output 'panel-process.exe'
    $panelLayerTest = Join-Path $repo 'tests\Test-PanelLayerHost.ps1'
    if (-not (Test-Path -LiteralPath $panelProcess)) { throw 'panel-process.exe was not built' }
    if (-not (Test-Path -LiteralPath $panelLayerTest)) { throw 'Test-PanelLayerHost.ps1 not found' }
    $hostExe = (Get-Process -Id $PID).Path
    & $hostExe -NoProfile -File $panelLayerTest `
        -PanelProcessExe $panelProcess `
        -ControlCenterScript (Join-Path $repo 'Dlss5.ControlCenter.ps1') `
        -ScratchRoot (Join-Path $output 'panel-layer-host')
    if ($LASTEXITCODE -ne 0) { throw "Panel layer host tests failed: $LASTEXITCODE" }
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $old[$name], 'Process') }
}
Write-Output "Test output: $output"
