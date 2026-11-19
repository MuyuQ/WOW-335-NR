<#
回归：Build-ControlCenter.ps1 的发布候选面板取件与身份校验（R7 / P-C）。

覆盖：
  A. 正确候选（_scratch 面板构建目录中的已验证 exe，哈希与 panel-lock.json 一致）：
     严格模式构建成功；包内 bin/dlss5-panel.exe 哈希等于锁值；
     build.json 的 panel_candidate.source_sha256 / packaged_sha256 / matches_lock、
     portable.panel_lock_match / watch_lock_match、lock_verification 与包内 panel-lock.json 均正确。
  B. 错误候选（默认取 tools/dlss5/bin/dlss5-panel.exe，即当前旧部署件）：
     严格模式非零退出，不创建输出目录、不产出可通过验收的包；
     构建前后部署目录逐文件哈希不变（构建不写 tools/dlss5/bin）。
  C. 参数/缺候选的快速失败（不编译）：
     C1 同时给 -PanelCandidate 与 -PanelCandidateDirectory；
     C2 严格模式不给候选。
  D. 可选对照（-IncludeUnverifiedContrast）：错误候选 + -AllowUnverified 的旧行为，
     产物标 unverified，仅作对照，不作为验收包。

用法（默认从 _scratch/dlss5-aio/panel-build-* 自动找哈希匹配锁值的候选）：
  pwsh -NoProfile -File tests/Test-ReleasePackaging.ps1
  pwsh -NoProfile -File tests/Test-ReleasePackaging.ps1 -CandidatePath D:\...\dlss5-panel.exe
  pwsh -NoProfile -File tests/Test-ReleasePackaging.ps1 -IncludeUnverifiedContrast
所有输出只写工作区 _scratch/dlss5-aio/release-packaging-test-<guid>/。
#>
param(
    [string]$CandidatePath,
    [string]$ExpectedPanelSha256,
    [string]$OldPanelPath,
    [switch]$IncludeUnverifiedContrast
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workspace = [IO.Path]::GetFullPath((Join-Path $repo '../..'))
$scratch = [IO.Path]::GetFullPath((Join-Path $workspace '_scratch'))
$buildScript = Join-Path $repo 'Build-ControlCenter.ps1'
$panelLockPath = Join-Path $repo 'panel-lock.json'
$toolsBin = Join-Path $workspace 'tools/dlss5/bin'

if (-not [IO.File]::Exists($buildScript)) { throw "缺少构建脚本：$buildScript" }
if (-not [IO.File]::Exists($panelLockPath)) { throw "缺少 panel-lock.json：$panelLockPath" }

$scratchPrefix = $scratch.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
$testRoot = Join-Path $scratch ('dlss5-aio/release-packaging-test-' + [guid]::NewGuid().ToString('N'))
$testRoot = [IO.Path]::GetFullPath($testRoot)
if (-not $testRoot.StartsWith($scratchPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw '测试输出必须位于 _scratch。'
}
$current = $testRoot
while ($current) {
    $item = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
    if ($null -ne $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "测试路径含链接：$current"
    }
    $parent = [IO.Path]::GetDirectoryName($current.TrimEnd([IO.Path]::DirectorySeparatorChar))
    if (-not $parent -or $parent -eq $current) { break }
    $current = $parent
}
[IO.Directory]::CreateDirectory($testRoot) | Out-Null
Write-Output ('测试证据目录: ' + $testRoot)

$script:Failures = [Collections.Generic.List[string]]::new()
$caseResults = [ordered]@{}

function Write-Case {
    param([string]$Text)
    Write-Output ''
    Write-Output ('== ' + $Text + ' ==')
}

function Assert-True {
    param([bool]$Condition, [string]$Description)
    if ($Condition) {
        Write-Output ('PASS  ' + $Description)
    } else {
        $script:Failures.Add($Description)
        Write-Output ('FAIL  ' + $Description)
    }
}

function Assert-Equal {
    param($Expected, $Actual, [string]$Description)
    $expectedText = if ($null -eq $Expected) { '<null>' } else { [string]$Expected }
    $actualText = if ($null -eq $Actual) { '<null>' } else { [string]$Actual }
    Assert-True -Condition ($expectedText -eq $actualText) `
        -Description ($Description + ' [期望 ' + $expectedText + ' 实际 ' + $actualText + ']')
}

function Get-Sha256Hex {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-JsonPath {
    param($Object, [string[]]$Path)
    $current = $Object
    foreach ($segment in $Path) {
        if ($null -eq $current) { return $null }
        $property = $current.PSObject.Properties[$segment]
        if ($null -eq $property) { return $null }
        $current = $property.Value
    }
    return $current
}

function Get-DirectorySnapshot {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return @() }
    return @(Get-ChildItem -LiteralPath $Path -File | Sort-Object Name | ForEach-Object {
            $_.Name + '|' + $_.Length + '|' + (Get-Sha256Hex -Path $_.FullName)
        })
}

function Invoke-BuildScript {
    param(
        [string[]]$Arguments,
        [string]$LogBase
    )
    $pwshPath = Join-Path $PSHOME 'pwsh.exe'
    if (-not [IO.File]::Exists($pwshPath)) { $pwshPath = (Get-Command -Name pwsh -ErrorAction Stop).Source }
    $argumentList = New-Object System.Collections.Generic.List[string]
    $argumentList.Add('-NoProfile')
    $argumentList.Add('-File')
    $argumentList.Add($buildScript)
    foreach ($argument in $Arguments) {
        if ($argument -match '[\s"]') { $argumentList.Add('"' + ($argument -replace '"', '\"') + '"') }
        else { $argumentList.Add($argument) }
    }
    $stdoutPath = $LogBase + '.stdout.log'
    $stderrPath = $LogBase + '.stderr.log'
    $process = Start-Process -FilePath $pwshPath -ArgumentList $argumentList.ToArray() -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
    $stdout = if (Test-Path -LiteralPath $stdoutPath) { [IO.File]::ReadAllText($stdoutPath) } else { '' }
    $stderr = if (Test-Path -LiteralPath $stderrPath) { [IO.File]::ReadAllText($stderrPath) } else { '' }
    return [pscustomobject]@{
        ExitCode   = $process.ExitCode
        StdOut     = $stdout
        StdErr     = $stderr
        StdOutPath = $stdoutPath
        StdErrPath = $stderrPath
    }
}

# ---------- 准备：锁值、候选、部署位快照 ----------
$lockRaw = Get-Content -LiteralPath $panelLockPath -Raw -Encoding UTF8 | ConvertFrom-Json
$lockExeHash = [string](Get-JsonPath -Object $lockRaw -Path @('files', 'dlss5-panel.exe'))
if ($lockExeHash -notmatch '^[0-9a-f]{64}$') { throw "panel-lock.json 的 dlss5-panel.exe 不是有效 SHA-256：$lockExeHash" }
$expected = if ($ExpectedPanelSha256) { $ExpectedPanelSha256.ToLowerInvariant() } else { $lockExeHash }
if ($expected -ne $lockExeHash) {
    throw "-ExpectedPanelSha256（$expected）与 panel-lock.json（$lockExeHash）不一致。"
}
Write-Output ('panel-lock.json 记录的面板哈希: ' + $lockExeHash)

function Find-CandidateByHash {
    param([string]$Hash)
    $root = Join-Path $scratch 'dlss5-aio'
    if (-not (Test-Path -LiteralPath $root)) { return $null }
    $directories = @(Get-ChildItem -LiteralPath $root -Directory -Filter 'panel-build-*' -ErrorAction SilentlyContinue |
                     Sort-Object LastWriteTime -Descending)
    foreach ($directory in $directories) {
        $path = Join-Path $directory.FullName 'dlss5-panel.exe'
        if ([IO.File]::Exists($path) -and (Get-Sha256Hex -Path $path) -eq $Hash) { return $path }
    }
    return $null
}

if ($CandidatePath) {
    $candidate = [IO.Path]::GetFullPath($CandidatePath)
    if (-not [IO.File]::Exists($candidate)) { throw "候选不存在：$candidate" }
    $candidateActual = Get-Sha256Hex -Path $candidate
    if ($candidateActual -ne $expected) { throw "候选哈希（$candidateActual）与 panel-lock.json（$expected）不一致：$candidate" }
} else {
    $candidate = Find-CandidateByHash -Hash $expected
    if ($null -eq $candidate) {
        throw "未在 _scratch/dlss5-aio/panel-build-* 找到哈希为 $expected 的候选，请用 -CandidatePath 指定。"
    }
}
Write-Output ('正确候选: ' + $candidate)

$deployedPanel = if ($OldPanelPath) { [IO.Path]::GetFullPath($OldPanelPath) } else { Join-Path $toolsBin 'dlss5-panel.exe' }
$deployedSnapshotBefore = Get-DirectorySnapshot -Path $toolsBin
Write-Output ('部署目录: ' + $toolsBin + '（构建前后逐文件哈希比对）')

# ---------- 用例 A：正确候选，严格模式 ----------
Write-Case '用例 A：正确候选（严格模式）'
$caseAOut = Join-Path $testRoot 'case-a-strict-candidate'
$resultA = Invoke-BuildScript -Arguments @('-OutputDirectory', $caseAOut, '-PanelCandidate', $candidate) `
    -LogBase (Join-Path $testRoot 'case-a')
Write-Output ('A exit code: ' + $resultA.ExitCode)
Write-Output ('A stdout: ' + $resultA.StdOutPath)
Write-Output ('A stderr: ' + $resultA.StdErrPath)
$caseResults['a_strict_candidate'] = [ordered]@{
    exit_code = $resultA.ExitCode
    output_directory = $caseAOut
    stdout = $resultA.StdOutPath
    stderr = $resultA.StdErrPath
}
Assert-True -Condition ($resultA.ExitCode -eq 0) -Description 'A: 正确候选严格构建退出码 0'
$buildJsonA = Join-Path $caseAOut 'build.json'
$panelExeA = Join-Path $caseAOut 'bin/dlss5-panel.exe'
Assert-True -Condition (Test-Path -LiteralPath $buildJsonA) -Description 'A: 生成 build.json'
Assert-True -Condition ([IO.File]::Exists($panelExeA)) -Description 'A: 包含 bin/dlss5-panel.exe'
if ([IO.File]::Exists($panelExeA)) {
    $packagedHash = Get-Sha256Hex -Path $panelExeA
    Write-Output ('A 包内面板 SHA-256: ' + $packagedHash)
    Assert-Equal -Expected $expected -Actual $packagedHash -Description 'A: 包内 EXE 哈希等于 panel-lock/候选'
    $caseResults['a_strict_candidate']['packaged_panel_sha256'] = $packagedHash
}
if (Test-Path -LiteralPath $buildJsonA) {
    $recordA = Get-Content -LiteralPath $buildJsonA -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-Equal -Expected 'compiled_candidate_native_ui' -Actual (Get-JsonPath -Object $recordA -Path @('state')) `
        -Description 'A: build.json.state'
    Assert-True -Condition ((Get-JsonPath -Object $recordA -Path @('unverified')) -eq $false) `
        -Description 'A: build.json.unverified=false'
    Assert-Equal -Expected $candidate -Actual (Get-JsonPath -Object $recordA -Path @('panel_candidate', 'source_path')) `
        -Description 'A: panel_candidate.source_path'
    Assert-Equal -Expected $expected -Actual (Get-JsonPath -Object $recordA -Path @('panel_candidate', 'source_sha256')) `
        -Description 'A: panel_candidate.source_sha256'
    Assert-Equal -Expected $expected -Actual (Get-JsonPath -Object $recordA -Path @('panel_candidate', 'packaged_sha256')) `
        -Description 'A: panel_candidate.packaged_sha256'
    Assert-True -Condition ((Get-JsonPath -Object $recordA -Path @('panel_candidate', 'matches_lock')) -eq $true) `
        -Description 'A: panel_candidate.matches_lock=true'
    Assert-True -Condition ((Get-JsonPath -Object $recordA -Path @('panel_candidate', 'packaged_matches_source')) -eq $true) `
        -Description 'A: panel_candidate.packaged_matches_source=true'
    Assert-True -Condition ((Get-JsonPath -Object $recordA -Path @('panel_candidate', 'panel_cpp', 'matches_lock')) -eq $true) `
        -Description 'A: panel_candidate.panel_cpp.matches_lock=true'
    Assert-True -Condition ((Get-JsonPath -Object $recordA -Path @('portable', 'panel_lock_match')) -eq $true) `
        -Description 'A: portable.panel_lock_match=true'
    Assert-True -Condition ((Get-JsonPath -Object $recordA -Path @('portable', 'watch_lock_match')) -eq $true) `
        -Description 'A: portable.watch_lock_match=true'
    Assert-True -Condition ((Get-JsonPath -Object $recordA -Path @('portable', 'lock_verification', 'verified')) -eq $true) `
        -Description 'A: portable.lock_verification.verified=true'
    Assert-Equal -Expected 'panel_candidate' `
        -Actual (Get-JsonPath -Object $recordA -Path @('portable', 'panel_lock', 'exe_source')) `
        -Description 'A: panel_lock.exe_source=panel_candidate'
    Assert-True -Condition ((Get-JsonPath -Object $recordA -Path @('portable', 'panel_lock', 'verified')) -eq $true) `
        -Description 'A: panel_lock.verified=true'
    Assert-Equal -Expected $expected `
        -Actual (Get-JsonPath -Object $recordA -Path @('portable', 'bundled_binaries', 'dlss5-panel.exe')) `
        -Description 'A: bundled_binaries 面板哈希'
    $packagedLockPath = Join-Path $caseAOut 'panel-lock.json'
    Assert-True -Condition ([IO.File]::Exists($packagedLockPath)) -Description 'A: 包内携带 panel-lock.json'
    if ([IO.File]::Exists($packagedLockPath)) {
        $packagedLock = Get-Content -LiteralPath $packagedLockPath -Raw -Encoding UTF8 | ConvertFrom-Json
        Assert-Equal -Expected $expected -Actual (Get-JsonPath -Object $packagedLock -Path @('files', 'dlss5-panel.exe')) `
            -Description 'A: 包内 panel-lock.json 面板哈希'
    }
}

# ---------- 用例 B：旧部署面板作为候选，严格模式必须失败 ----------
Write-Case '用例 B：错误候选（旧部署面板，严格模式必须失败）'
$caseResults['b_reject_deployed_panel'] = [ordered]@{ exit_code = $null; output_directory = $null; skipped = $null }
if (-not [IO.File]::Exists($deployedPanel)) {
    Write-Output ('SKIP  旧面板不存在：' + $deployedPanel)
    $caseResults['b_reject_deployed_panel']['skipped'] = '旧面板不存在'
} elseif ((Get-Sha256Hex -Path $deployedPanel) -eq $expected) {
    Write-Output 'SKIP  部署位面板已是候选哈希，无法作为错误样本（可用 -OldPanelPath 指定旧件）'
    $caseResults['b_reject_deployed_panel']['skipped'] = '部署位面板已是候选哈希'
} else {
    $oldHash = Get-Sha256Hex -Path $deployedPanel
    Write-Output ('B 旧面板 SHA-256: ' + $oldHash)
    $caseBOut = Join-Path $testRoot 'case-b-reject-deployed-panel'
    $resultB = Invoke-BuildScript -Arguments @('-OutputDirectory', $caseBOut, '-PanelCandidate', $deployedPanel) `
        -LogBase (Join-Path $testRoot 'case-b')
    Write-Output ('B exit code: ' + $resultB.ExitCode)
    Write-Output ('B stdout: ' + $resultB.StdOutPath)
    Write-Output ('B stderr: ' + $resultB.StdErrPath)
    $combinedB = [string]$resultB.StdOut + "`n" + [string]$resultB.StdErr
    Assert-True -Condition ($resultB.ExitCode -ne 0) -Description 'B: 旧面板候选使严格构建非零退出'
    Assert-True -Condition (-not (Test-Path -LiteralPath $caseBOut)) -Description 'B: 失败时不创建输出目录（不产出包）'
    Assert-True -Condition ($combinedB -match 'panel-lock\.json') -Description 'B: 失败信息定位 panel-lock.json'
    Assert-True -Condition ($combinedB -match 'SHA-256') -Description 'B: 失败信息包含哈希不符说明'
    $caseResults['b_reject_deployed_panel']['exit_code'] = $resultB.ExitCode
    $caseResults['b_reject_deployed_panel']['output_directory'] = $caseBOut
    $caseResults['b_reject_deployed_panel']['old_panel_sha256'] = $oldHash
    $caseResults['b_reject_deployed_panel']['output_directory_created'] = (Test-Path -LiteralPath $caseBOut)
}

# ---------- 用例 C：参数与缺候选快速失败（不编译） ----------
Write-Case '用例 C：参数/缺候选快速失败（不编译）'
$caseCOut = Join-Path $testRoot 'case-c-conflicting-arguments'
$resultC = Invoke-BuildScript -Arguments @('-OutputDirectory', $caseCOut, '-PanelCandidate', $candidate, `
        '-PanelCandidateDirectory', (Split-Path -Parent $candidate)) -LogBase (Join-Path $testRoot 'case-c')
$combinedC = [string]$resultC.StdOut + "`n" + [string]$resultC.StdErr
Assert-True -Condition ($resultC.ExitCode -ne 0) -Description 'C1: 互斥参数非零退出'
Assert-True -Condition (-not (Test-Path -LiteralPath $caseCOut)) -Description 'C1: 互斥参数不创建输出目录'
Assert-True -Condition ($combinedC -match 'PanelCandidateDirectory') -Description 'C1: 失败信息指出互斥参数'
$caseResults['c_conflicting_arguments'] = [ordered]@{
    exit_code = $resultC.ExitCode
    output_directory = $caseCOut
    output_directory_created = (Test-Path -LiteralPath $caseCOut)
}

$caseC2Out = Join-Path $testRoot 'case-c-missing-candidate'
$resultC2 = Invoke-BuildScript -Arguments @('-OutputDirectory', $caseC2Out) -LogBase (Join-Path $testRoot 'case-c2')
$combinedC2 = [string]$resultC2.StdOut + "`n" + [string]$resultC2.StdErr
Assert-True -Condition ($resultC2.ExitCode -ne 0) -Description 'C2: 严格模式缺少候选非零退出'
Assert-True -Condition (-not (Test-Path -LiteralPath $caseC2Out)) -Description 'C2: 缺少候选不创建输出目录'
Assert-True -Condition ($combinedC2 -match 'PanelCandidate') -Description 'C2: 失败信息提示 -PanelCandidate'
$caseResults['c_missing_candidate'] = [ordered]@{
    exit_code = $resultC2.ExitCode
    output_directory = $caseC2Out
    output_directory_created = (Test-Path -LiteralPath $caseC2Out)
}

# ---------- 用例 D（可选对照）：错误候选 + -AllowUnverified ----------
if ($IncludeUnverifiedContrast) {
    Write-Case '用例 D（对照）：错误候选 + -AllowUnverified（旧行为，不可验收）'
    if (-not [IO.File]::Exists($deployedPanel) -or (Get-Sha256Hex -Path $deployedPanel) -eq $expected) {
        Write-Output 'SKIP  没有可用的旧面板样本，跳过对照用例 D'
    } else {
        $caseDOut = Join-Path $testRoot 'case-d-allow-unverified'
        $resultD = Invoke-BuildScript -Arguments @('-OutputDirectory', $caseDOut, '-PanelCandidate', $deployedPanel, `
                '-AllowUnverified') -LogBase (Join-Path $testRoot 'case-d')
        $buildJsonD = Join-Path $caseDOut 'build.json'
        Assert-True -Condition ($resultD.ExitCode -eq 0) -Description 'D: -AllowUnverified 下旧候选可出包（仅对照）'
        if (Test-Path -LiteralPath $buildJsonD) {
            $recordD = Get-Content -LiteralPath $buildJsonD -Raw -Encoding UTF8 | ConvertFrom-Json
            Assert-Equal -Expected 'compiled_candidate_native_ui_unverified' `
                -Actual (Get-JsonPath -Object $recordD -Path @('state')) -Description 'D: 对照包状态为 unverified'
            Assert-True -Condition ((Get-JsonPath -Object $recordD -Path @('panel_candidate', 'matches_lock')) -eq $false) `
                -Description 'D: 对照包 matches_lock=false'
            Assert-True -Condition ((Get-JsonPath -Object $recordD -Path @('portable', 'lock_verification', 'allow_unverified')) -eq $true) `
                -Description 'D: 对照包 allow_unverified=true'
        }
        $caseResults['d_allow_unverified_contrast'] = [ordered]@{
            exit_code = $resultD.ExitCode
            output_directory = $caseDOut
            note = '仅对照旧行为，不作为验收包'
        }
    }
}

# ---------- 部署目录未被改写 ----------
Write-Case '部署目录完整性'
$deployedSnapshotAfter = Get-DirectorySnapshot -Path $toolsBin
Assert-Equal -Expected ($deployedSnapshotBefore -join ';') -Actual ($deployedSnapshotAfter -join ';') `
    -Description '部署目录 tools/dlss5/bin 未被构建改写（逐文件哈希）'

# ---------- 结果汇总 ----------
$passed = ($script:Failures.Count -eq 0)
$summary = [ordered]@{
    schema = 1
    generated_utc = [DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    repository = $repo
    workspace = $workspace
    lock_panel_sha256 = $lockExeHash
    expected_panel_sha256 = $expected
    candidate_path = $candidate
    deployed_panel_path = $deployedPanel
    cases = $caseResults
    failures = @($script:Failures)
    passed = $passed
}
$summaryPath = Join-Path $testRoot 'result.json'
[IO.File]::WriteAllText($summaryPath, (($summary | ConvertTo-Json -Depth 8) + "`n"), [Text.UTF8Encoding]::new($false))
Write-Output ''
Write-Output ('结果汇总: ' + $summaryPath)
if ($passed) {
    Write-Output ('ALL PASS（0 失败）证据目录: ' + $testRoot)
    exit 0
}
Write-Output ('FAILED（' + $script:Failures.Count + ' 项失败）:')
foreach ($failure in $script:Failures) { Write-Output ('  - ' + $failure) }
Write-Output ('证据目录: ' + $testRoot)
exit 1
