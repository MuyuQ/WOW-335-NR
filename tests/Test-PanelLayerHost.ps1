#requires -Version 7.0
<#
.SYNOPSIS
    P2a 回归：控制中心“图层”按钮必须只把 WM_APP+0x5A 发给“所选客户端”的 AIO 载具。

.DESCRIPTION
    1. 从生产脚本 Dlss5.ControlCenter.ps1 中提取 Get-AioHostId（不加载 UI），
       验证它按所选客户端的 host64\AIO DLSS5 32-bit Wrapper.exe 完整映像路径定 PID。
    2. 用 panel-process.exe 的假面板模式伪造两个“AIO 载具”进程：
       它们创建 StandaloneDLSSNRNativeOutput 输出窗，并把收到的 WM_APP+0x5A 记入日志。
    3. 断言：选 A、只运行 B 时 Get-AioHostId 返回 0（点击回调会直接报错，不发消息）；
       选 A、A/B 同时运行时只解析到 A，且 A 恰好收到 1 条消息、B 收到 0 条。

    本脚本只用隔离夹具目录与假进程，不接触真实游戏、真实载具或真实面板。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$PanelProcessExe,
    [Parameter(Mandatory = $true)][string]$ControlCenterScript,
    [Parameter(Mandatory = $true)][string]$ScratchRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:failures = 0
function Check([bool]$Condition, [string]$Label) {
    if (-not $Condition) {
        $script:failures++
        Write-Host "FAIL $Label"
    }
}

function Wait-Exists([string]$Path, [int]$TimeoutMs = 10000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        if (Test-Path -LiteralPath $Path) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

function Stop-Quietly($Process) {
    if ($null -eq $Process) { return }
    try { if (-not $Process.HasExited) { [void]$Process.Kill($true) } } catch { }
    try { [void]$Process.WaitForExit(3000) } catch { }
}

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$panelProcessExe = [IO.Path]::GetFullPath($PanelProcessExe)
$controlCenterScript = [IO.Path]::GetFullPath($ControlCenterScript)
if (-not (Test-Path -LiteralPath $panelProcessExe)) { throw "panel-process.exe not found: $panelProcessExe" }
if (-not (Test-Path -LiteralPath $controlCenterScript)) { throw "Dlss5.ControlCenter.ps1 not found: $controlCenterScript" }

$root = Join-Path ([IO.Path]::GetFullPath($ScratchRoot)) 'panel-layer-host'
if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
[IO.Directory]::CreateDirectory($root) | Out-Null

$clientA = Join-Path $root 'client-a'
$clientB = Join-Path $root 'client-b'
foreach ($client in @($clientA, $clientB)) {
    [IO.Directory]::CreateDirectory((Join-Path $client 'host64')) | Out-Null
}
$gameA = Join-Path $clientA 'wow.exe'
$gameB = Join-Path $clientB 'wow.exe'
$hostA = Join-Path $clientA 'host64\AIO DLSS5 32-bit Wrapper.exe'
$hostB = Join-Path $clientB 'host64\AIO DLSS5 32-bit Wrapper.exe'
Copy-Item -LiteralPath $panelProcessExe -Destination $hostA -Force
Copy-Item -LiteralPath $panelProcessExe -Destination $hostB -Force

$logA = Join-Path $root 'carrier-a-messages.txt'
$logB = Join-Path $root 'carrier-b-messages.txt'
$readyA = Join-Path $root 'carrier-a-ready.txt'
$readyB = Join-Path $root 'carrier-b-ready.txt'

# --- 1) 提取生产脚本里的 Get-AioHostId，放进一个只带 clientPath 变量的动态模块 ---
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($controlCenterScript, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -gt 0) { throw "Dlss5.ControlCenter.ps1 has parse errors: $($parseErrors[0].Message)" }
$functionAst = $ast.Find({
        param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-AioHostId'
    }, $true)
if ($null -eq $functionAst) { throw 'Get-AioHostId not found in Dlss5.ControlCenter.ps1' }
$moduleSource = $functionAst.Extent.Text + "`nfunction Set-FixtureClientPath([string]`$Path) { `$script:clientPath = `$Path }"
$module = New-Module -Name 'PanelLayerHostFixture' -ScriptBlock ([scriptblock]::Create($moduleSource))
Import-Module $module -Force
$hostIdFunction = Get-Command -Name Get-AioHostId -Module 'PanelLayerHostFixture'
if ($null -eq $hostIdFunction) { throw 'Get-AioHostId was not imported from the fixture module' }

# --- 2) 加载生产脚本里的 C# PanelLayerNative（消息发送实现） ---
$scriptText = [IO.File]::ReadAllText($controlCenterScript)
$addTypeBlocks = [regex]::Matches($scriptText, "(?s)Add-Type -TypeDefinition @'\r?\n(?<source>.*?)\r?\n'@")
$layerSource = $null
foreach ($match in $addTypeBlocks) {
    if ($match.Groups['source'].Value -match 'PanelLayerNative') { $layerSource = $match.Groups['source'].Value; break }
}
if ($null -eq $layerSource) { throw 'PanelLayerNative C# source not found in Dlss5.ControlCenter.ps1' }
if (-not ('Wow335Ui.PanelLayerNative' -as [type])) {
    Add-Type -TypeDefinition $layerSource
}

# --- 3) 先只启动 B 的“载具”，选 A 时不得解析到任何宿主 ---
$processB = $null
$processA = $null
try {
    $processB = Start-Process -FilePath $hostB -WindowStyle Hidden -PassThru -ArgumentList @(
        '--fake-panel', '--game', ('"' + $gameB + '"'), '--message-log', ('"' + $logB + '"'), '--ready', ('"' + $readyB + '"'))
    Check (Wait-Exists $readyB) 'carrier B fixture created its output window'

    Set-FixtureClientPath $clientA
    $hostId = & $hostIdFunction
    Check ($hostId -eq 0) 'selecting client A ignores carrier B on the same machine'

    # --- 4) 再启动 A 的“载具”：只解析到 A，消息只到 A ---
    $processA = Start-Process -FilePath $hostA -WindowStyle Hidden -PassThru -ArgumentList @(
        '--fake-panel', '--game', ('"' + $gameA + '"'), '--message-log', ('"' + $logA + '"'), '--ready', ('"' + $readyA + '"'))
    Check (Wait-Exists $readyA) 'carrier A fixture created its output window'

    $hostId = & $hostIdFunction
    Check ($hostId -eq $processA.Id) 'selecting client A resolves only carrier A'

    $sendError = 0
    $sent = [Wow335Ui.PanelLayerNative]::TrySetShowProcessed([uint32]$hostId, $true, [ref]$sendError)
    Check $sent "show-processed request accepted for the selected client's carrier (error $sendError)"

    # 轮询等待消息被记录（fake carrier 在消息循环里写文件）。
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt 3000) {
        $linesA = @(Get-Content -LiteralPath $logA -ErrorAction SilentlyContinue)
        if ($linesA.Count -ge 1) { break }
        Start-Sleep -Milliseconds 100
    }
    $linesA = @(Get-Content -LiteralPath $logA -ErrorAction SilentlyContinue)
    $linesB = @(Get-Content -LiteralPath $logB -ErrorAction SilentlyContinue)
    Check ($linesA.Count -eq 1) "selected carrier received exactly one message (actual $($linesA.Count))"
    if ($linesA.Count -eq 1) { Check ($linesA[0].Trim() -eq '1') 'selected carrier received the requested show state' }
    Check ($linesB.Count -eq 0) "other client's carrier received no message (actual $($linesB.Count))"
} finally {
    Stop-Quietly $processA
    Stop-Quietly $processB
    try { Remove-Module -Name 'PanelLayerHostFixture' -Force -ErrorAction SilentlyContinue } catch { }
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}

if ($script:failures -ne 0) {
    Write-Output "panel-layer-host: $($script:failures) failure(s)"
    exit 1
}
Write-Output 'panel-layer-host: selected-client carrier selection and message routing passed'
exit 0
