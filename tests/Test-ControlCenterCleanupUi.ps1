# 控制中心「重试清理」按钮与结果卡的隔离 UI 回归。
# 方法：读取生产脚本 Dlss5.ControlCenter.ps1，截取到 Show-View 'main' 之前（保留全部函数、
# 窗体与页面构建代码），在同一个脚本作用域末尾追加驱动代码；仅把磁盘清理调用
# Invoke-Dlss5CleanupPending 替换为内存桩，按钮点击走真实的 WinForms PerformClick 与生产
# Add_Click 回调（Invoke-Ui、Refresh-Status、Update-Maint 全部是生产实现）。
# 断言覆盖：无恢复结果→按钮禁用；首次恢复留下待清理→启用且 SavedModified 显示为整数；
# 第一次点击仍失败→页面刷新、原因可见、按钮可再点；第二次点击成功→页面刷新、按钮禁用；
# 永久失效→按钮禁用并显示原因；回调不得被 Invoke-Ui 捕获到“操作未完成”异常。
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$controlCenter = Join-Path $repo 'Dlss5.ControlCenter.ps1'
if (-not (Test-Path -LiteralPath $controlCenter -PathType Leaf)) { throw "缺少生产脚本：$controlCenter" }
$testRoot = Join-Path $repo ('_scratch/dlss5-aio/ui-cleanup-test-' + [guid]::NewGuid().ToString('N'))
if (Test-Path -LiteralPath $testRoot) { throw "测试目录已存在：$testRoot" }
[IO.Directory]::CreateDirectory($testRoot) | Out-Null
$client = Join-Path $testRoot 'client'
[IO.Directory]::CreateDirectory($client) | Out-Null
# 夹具客户端：只要满足控制中心页面对 wow.exe / d3d9.dll 的只读状态检查即可。
$bytes = [byte[]]::new(512)
$bytes[0] = 0x4d; $bytes[1] = 0x5a
$bytes[0x3c] = 0x80
$bytes[0x80] = 0x50; $bytes[0x81] = 0x45
$bytes[0x84] = 0x4c; $bytes[0x85] = 0x01
[IO.File]::WriteAllBytes((Join-Path $client 'wow.exe'), $bytes)
[IO.File]::WriteAllText((Join-Path $client 'd3d9.dll'), 'original-dxvk-fixture', [Text.UTF8Encoding]::new($false))

$source = [IO.File]::ReadAllText($controlCenter, [Text.UTF8Encoding]::new($false, $true))
$marker = "`nShow-View 'main'`n"
$index = $source.IndexOf($marker, [StringComparison]::Ordinal)
if ($index -lt 0) { throw "未能在生产脚本中定位 Show-View 'main' 标记。" }
$prefix = $source.Substring(0, $index)
$prefix = $prefix.Replace('$PSScriptRoot', ("'" + $repo + "'"))

$harness = @'
# ==== 隔离 UI 测试驱动（仅替换磁盘清理调用，其余均为生产代码） ====
$uiTimer.Stop()
$watchTimer.Stop()
# Button.PerformClick() 只在控件可选中（Enabled 且 Visible）时触发 Click，因此必须真正显示窗体；
# 为了让测试窗口不干扰桌面，移到屏幕外。窗体的 ShowDialog 已在截断点之后，不会进入消息循环。
$form.StartPosition = [Windows.Forms.FormStartPosition]::Manual
$form.Location = [Drawing.Point]::new(-32000, -32000)
$form.Show()
[Windows.Forms.Application]::DoEvents()
# 维护页的「高级工具」区块（含恢复/重试清理按钮与恢复结果卡）由 showAdvanced 控制，默认关闭；
# 生产界面里由设置里的开关打开。这里显式打开，才能走到被测的按钮与结果卡。
$script:showAdvanced = $true
function Get-UiControlsDeep([object]$Root) {
    $collected = @()
    foreach ($child in $Root.Controls) {
        $collected += $child
        $collected += Get-UiControlsDeep $child
    }
    return $collected
}
function Get-UiButton([string]$Text) {
    $matches = @(Get-UiControlsDeep $form | Where-Object { $_ -is [Windows.Forms.Button] -and [string]$_.Text -eq $Text })
    if ($matches.Count -eq 0) { return $null }
    return $matches[0]
}
function Get-UiLabelTexts {
    return @(Get-UiControlsDeep $form | Where-Object { $_ -is [Windows.Forms.Label] } | ForEach-Object { [string]$_.Text })
}
function Test-UiLabelLike([string]$Pattern) {
    return @(Get-UiLabelTexts | Where-Object { $_ -like $Pattern }).Count -gt 0
}
$script:uiStubCleanupResult = $null
$script:uiStubCalls = 0
function Invoke-Dlss5CleanupPending {
    param([string]$Operation, [switch]$Recovery)
    $script:uiStubCalls++
    if ($null -eq $script:uiStubCleanupResult) { throw 'UI 测试桩未设置清理结果。' }
    return $script:uiStubCleanupResult
}
$script:failures = New-Object 'System.Collections.Generic.List[string]'
function Check-Ui([bool]$Condition, [string]$Message) {
    if (-not $Condition) { $script:failures.Add($Message) }
}
function Update-MaintView {
    $script:viewDirty['maint'] = $true
    Show-View 'maint'
}
# 记录每一步按钮/结果卡的可核对状态（交付证据与以后排查用）。
$script:observations = [ordered]@{}
function Record-Observation([string]$Name) {
    $button = Get-UiButton '重试清理'
    $script:observations[$Name] = [ordered]@{
        restoreStatus = if ($null -ne $script:restoreResult) { [string]$script:restoreResult.Status } else { $null }
        buttonPresent = ($null -ne $button)
        buttonEnabled = if ($null -ne $button) { [bool]$button.Enabled } else { $null }
        buttonInstance = if ($null -ne $button) { [string]$button.GetHashCode() } else { $null }
        labels = @(Get-UiLabelTexts)
    }
}
try {
    # 1) 无恢复结果：按钮不存在可重试事务，必须禁用，也不显示恢复结果卡。
    $script:restoreResult = $null
    Update-MaintView
    Record-Observation 'no-restore-result'
    $button = Get-UiButton '重试清理'
    Check-Ui ($null -ne $button) '无恢复结果时未找到重试清理按钮'
    if ($null -ne $button) { Check-Ui (-not $button.Enabled) '无恢复结果时重试清理按钮应禁用' }
    Check-Ui (-not (Test-UiLabelLike '恢复结果')) '无恢复结果时不应显示恢复结果卡'

    # 2) 首次恢复留下待清理：按钮启用，结果卡按整数显示 SavedModified。
    $script:restoreResult = [pscustomobject]@{ Status = 'rolled_back'; Operation = 'op-ui';
        SavedModified = 2; ModifiedRoot = 'C:\kept-files'; Report = '已回滚'; Errors = @();
        CleanupIncomplete = $true; CleanupErrors = @('安装记录文件被占用'); CleanupPermanent = $false }
    Update-MaintView
    Record-Observation 'pending-cleanup'
    $firstButton = Get-UiButton '重试清理'
    Check-Ui ($null -ne $firstButton) '首次恢复后未找到重试清理按钮'
    if ($null -ne $firstButton) { Check-Ui ([bool]$firstButton.Enabled) '首次恢复留下待清理时按钮应启用' }
    Check-Ui (Test-UiLabelLike '*2 项*') '结果卡未按整数显示 SavedModified=2'

    # 3) 第一次点击仍失败：页面必须刷新、原因可见、按钮仍可点。
    $script:uiStubCleanupResult = [pscustomobject]@{ Status = 'cleanup_incomplete'; Operation = 'op-ui';
        RemovedBackups = 0; BackupsKept = @(); Errors = @(); CleanupIncomplete = $true;
        CleanupPermanent = $false; CleanupErrors = @('安装记录文件被占用');
        Message = '清理仍有未完成项，请解除文件占用后重试。' }
    if ($null -ne $firstButton) { $firstButton.PerformClick() }
    Record-Observation 'after-failed-click'
    Check-Ui (Test-UiLabelLike '*未完成（可重试清理）*') '首次失败后结果卡未显示可重试状态'
    Check-Ui (Test-UiLabelLike '*清理说明：*') '首次失败后未显示可见原因'
    Check-Ui (Test-UiLabelLike '*安装记录文件被占用*') '首次失败后未显示错误原因'
    $secondButton = Get-UiButton '重试清理'
    Check-Ui ($null -ne $secondButton) '首次失败后未重建重试清理按钮'
    if ($null -ne $secondButton) { Check-Ui ([bool]$secondButton.Enabled) '首次失败后按钮应保持可用' }
    Check-Ui (-not [object]::ReferenceEquals($firstButton, $secondButton)) '首次失败后页面未刷新（按钮实例未重建）'

    # 4) 第二次点击成功：页面刷新、按钮禁用。
    $script:uiStubCleanupResult = [pscustomobject]@{ Status = 'cleaned'; Operation = 'op-ui';
        RemovedBackups = 1; BackupsKept = @(); Errors = @(); CleanupIncomplete = $false;
        CleanupPermanent = $false; CleanupErrors = @(); Message = '清理完成。' }
    if ($null -ne $secondButton) { $secondButton.PerformClick() }
    Record-Observation 'after-successful-click'
    Check-Ui (Test-UiLabelLike '*完成*') '成功后结果卡未显示完成'
    $thirdButton = Get-UiButton '重试清理'
    Check-Ui ($null -ne $thirdButton) '成功后未重建重试清理按钮'
    if ($null -ne $thirdButton) { Check-Ui (-not $thirdButton.Enabled) '成功后重试清理按钮应禁用' }
    Check-Ui (-not [object]::ReferenceEquals($secondButton, $thirdButton)) '成功后页面未刷新（按钮实例未重建）'

    # 5) 永久失效：按钮禁用并显示原因。
    $script:restoreResult = Get-Dlss5CleanupRetryState -Previous $script:restoreResult -Result ([pscustomobject]@{
        Status = 'superseded'; Permanent = $true; Operation = 'op-ui'; SupersededBy = 'op-new'; Errors = @() })
    Update-MaintView
    Record-Observation 'permanent-superseded'
    $permanentButton = Get-UiButton '重试清理'
    Check-Ui ($null -ne $permanentButton) '永久失效后未找到重试清理按钮'
    if ($null -ne $permanentButton) { Check-Ui (-not $permanentButton.Enabled) '永久失效后重试清理按钮应禁用' }
    Check-Ui (Test-UiLabelLike '*已结束（不可重试）*') '永久失效后未显示结束状态'
    Check-Ui (Test-UiLabelLike '*清理说明：*') '永久失效后未显示原因'
    Check-Ui (-not (Test-UiLabelLike '收尾错误：')) '永久失效且无清理错误时不应渲染空的收尾错误行'

    # 6) 生产回调不得抛出被 Invoke-Ui 捕获的异常。
    Check-Ui (-not ([string]$script:logText -like '*操作未完成*')) '回调抛出了被 Invoke-Ui 捕获的异常'
} catch {
    $script:failures.Add('未捕获异常：' + $_.Exception.Message) | Out-Null
}
if ($script:failures.Count -eq 0) {
    $stateFile = [Environment]::GetEnvironmentVariable('UI_TEST_STATE_FILE')
    if ($stateFile) {
        [IO.File]::WriteAllText($stateFile, (ConvertTo-Json $script:observations -Depth 6), [Text.UTF8Encoding]::new($false))
    }
    Write-Output 'UI cleanup test passed'
    $form.Dispose()
    exit 0
}
foreach ($failure in $script:failures) { [Console]::Error.WriteLine('UI cleanup test failed: ' + $failure) }
$form.Dispose()
exit 1
'@

$generated = Join-Path $testRoot 'ControlCenterUiHarness.ps1'
[IO.File]::WriteAllText($generated, ($prefix + "`n" + $harness), [Text.UTF8Encoding]::new($true))
$powershell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
if (-not (Test-Path -LiteralPath $powershell)) { throw "缺少 Windows PowerShell：$powershell" }
$logPath = Join-Path $testRoot 'ui-test.log'
$statePath = Join-Path $testRoot 'ui-states.json'
$env:UI_TEST_STATE_FILE = $statePath
try {
    $output = & $powershell -NoProfile -STA -ExecutionPolicy Bypass -File $generated -Client $client 2>&1 | Out-String
    $code = $LASTEXITCODE
} finally {
    Remove-Item Env:UI_TEST_STATE_FILE -ErrorAction SilentlyContinue
}
[IO.File]::WriteAllText($logPath, $output, [Text.UTF8Encoding]::new($false))
if ($code -ne 0) { throw ("控制中心重试清理 UI 回归失败（exit {0}）：`n{1}`n证据：{2}" -f $code, $output, $logPath) }
if ($output -notlike '*UI cleanup test passed*') { throw ("控制中心重试清理 UI 回归缺少通过标记：`n{0}`n证据：{1}" -f $output, $logPath) }
$states = [IO.File]::ReadAllText($statePath, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json
foreach ($name in @('no-restore-result', 'pending-cleanup', 'after-failed-click', 'after-successful-click', 'permanent-superseded')) {
    if ($null -eq $states.PSObject.Properties[$name]) { throw "UI 状态证据缺少步骤：$name" }
}
Write-Host ("控制中心重试清理 UI 回归通过；证据目录 {0}；状态证据 {1}" -f $testRoot, $statePath)
