Set-StrictMode -Version Latest
$script:ModuleRoot = $PSScriptRoot
$script:ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$script:ScratchRoot = [IO.Path]::GetFullPath((Join-Path $script:ProjectRoot '_scratch'))
$script:OperationsRoot = [IO.Path]::GetFullPath((Join-Path $script:ScratchRoot 'dlss5-aio/control-operations'))
$script:LockFile = Join-Path $PSScriptRoot 'payload-lock.json'
# zh: 安装清单——「哪些文件会进入客户端目录、装到哪、装前该是什么状态」的唯一事实源。
# 先于安装建立并随组件维护；与 payload-lock.json 的成员一一对应，缺项/多项直接报错（20261112）。
$script:ManifestFile = Join-Path $PSScriptRoot 'install-manifest.json'
# zh: 只读基准——安装器拒绝把组件写进原版分发基准与 reference。
$script:ProtectedRoots = @(
    (Join-Path $script:ProjectRoot 'zhcn-case-v2/14-template/original-distribution-baseline-202604'),
    (Join-Path $script:ProjectRoot 'reference')
)
# zh: 调试追踪——环境变量 DLSS5_CORE_DEBUG=1 时把事务关键步骤写入
# _scratch/dlss5-aio/logs/core-debug.log（20261109）。日志文件不入库（logs/）。
$script:DebugEnabled = ($env:DLSS5_CORE_DEBUG -eq '1')
$script:DebugLogPath = [IO.Path]::GetFullPath((Join-Path $script:ScratchRoot 'dlss5-aio/logs/core-debug.log'))
function Write-Dlss5CoreDebug {
    param([Parameter(Mandatory)][string]$Message)
    if (-not $script:DebugEnabled) { return }
    try {
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($script:DebugLogPath)) | Out-Null
        $stamp = [DateTime]::Now.ToString('HH:mm:ss.fff')
        [IO.File]::AppendAllText($script:DebugLogPath, "$stamp $Message`r`n", [Text.UTF8Encoding]::new($false))
    } catch { }
}
if (-not ('Wow335.Dlss5.FileApi' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Wow335.Dlss5 {
    public static class FileApi {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern bool MoveFileEx(string source, string destination, uint flags);
    }
}
'@
}

function Get-CanonicalPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or -not [IO.Path]::IsPathRooted($Path)) {
        throw '路径必须是绝对路径。'
    }
    return [IO.Path]::GetFullPath($Path).TrimEnd([IO.Path]::DirectorySeparatorChar)
}

function Test-Within([string]$Path, [string]$Root) {
    $pathFull = Get-CanonicalPath $Path
    $rootFull = Get-CanonicalPath $Root
    return $pathFull.Equals($rootFull, [StringComparison]::OrdinalIgnoreCase) -or
        $pathFull.StartsWith($rootFull + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
}

function Assert-NoReparse([string]$Path) {
    $current = [IO.Path]::GetFullPath($Path)
    while (-not [string]::IsNullOrEmpty($current)) {
        $item = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
        if ($null -ne $item) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "路径含有链接或重解析点：$current"
            }
        }
        $parent = [IO.Path]::GetDirectoryName($current.TrimEnd([IO.Path]::DirectorySeparatorChar))
        if ([string]::IsNullOrEmpty($parent) -or $parent -eq $current) { break }
        $current = $parent
    }
}

function Get-MemberPath([string]$Root, [string]$Relative) {
    if ([string]::IsNullOrWhiteSpace($Relative) -or [IO.Path]::IsPathRooted($Relative) -or
        $Relative.Contains(':') -or $Relative -match '[\x00-\x1f]') { throw '成员路径无效。' }
    $parts = $Relative -split '[/\\]'
    if (@($parts | Where-Object { $_ -in @('', '.', '..') }).Count -gt 0) { throw '成员路径无效。' }
    $path = Get-CanonicalPath (Join-Path $Root ($parts -join [IO.Path]::DirectorySeparatorChar))
    if (-not (Test-Within $path $Root) -or $path -eq (Get-CanonicalPath $Root)) { throw '成员路径越界。' }
    Assert-NoReparse $path
    return $path
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash.ToLowerInvariant()
}

function Move-AtomicReplace([string]$Source, [string]$Target) {
    $sourceFull = Get-CanonicalPath $Source
    $targetFull = Get-CanonicalPath $Target
    if (-not [IO.Path]::GetDirectoryName($sourceFull).Equals([IO.Path]::GetDirectoryName($targetFull),
        [StringComparison]::OrdinalIgnoreCase)) { throw '原子替换要求源与目标在同一目录。' }
    Assert-NoReparse $sourceFull
    Assert-NoReparse $targetFull
    if (-not [IO.File]::Exists($sourceFull) -or -not [IO.File]::Exists($targetFull)) {
        throw '原子替换源或目标缺失。'
    }
    if (-not [Wow335.Dlss5.FileApi]::MoveFileEx($sourceFull, $targetFull, 0x1 -bor 0x8)) {
        throw [ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error())
    }
}

function Get-Dlss5Lock {
    $lock = [IO.File]::ReadAllText($script:LockFile, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json
    # zh: 成员数量不设硬编码——数量随锁内容走，结构性校验在此处完成（20261109）。
    if ($lock.schema -ne 1 -or @($lock.members).Count -lt 1) { throw '组件清单格式不受支持。' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($member in $lock.members) {
        if (-not $seen.Add([string]$member.target)) { throw '组件清单中有重复目标。' }
        if ($member.sha256 -notmatch '^[0-9a-f]{64}$' -or $member.source_kind -notin @('stage', 'template', 'runtime')) {
            throw '组件清单包含无效身份。'
        }
        $first = ([string]$member.target -split '[/\\]')[0]
        if ($first -in @('WTF', 'Cache', 'Logs', 'Screenshots', 'Data', 'Interface', 'Fonts')) {
            throw '组件清单触及受保护目录。'
        }
    }
    # zh: 不再要求 game_sha256 / original_d3d9_sha256 匹配（20261112 用户决定）。
    # 理由：存在官方与第三方修改过的 wow.exe，哈希锁定会把这类客户端整体拒掉。
    # 当前身份口径：wow.exe 只校验存在 + x86 架构；d3d9.dll 在安装时按实际内容快照
    # （存在则备份，不存在则记为 absent），卸载按该快照恢复或删除。
    $replaced = @($lock.members | Where-Object { $_.replaces_original })
    if ($replaced.Count -gt 1 -or ($replaced.Count -eq 1 -and [string]$replaced[0].target -ne 'd3d9.dll')) {
        throw '原件替换清单无效。'
    }
    return $lock
}

function Get-ManifestField($Entry, [string]$Name) {
    # zh: 清单条目字段可能缺失（手写 JSON）；Set-StrictMode 下直接访问缺失属性会抛错，
    # 统一从 PSObject.Properties 安全取值，缺失返回 $null（20261112）。
    if ($null -eq $Entry) { return $null }
    $property = $Entry.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Get-Dlss5Manifest {
    # zh: 安装清单是「哪些文件进客户端、装到哪个目录、装前该是什么状态、卸载删哪些残留」的唯一事实源。
    # 先于安装建立并随组件维护；必须与 payload-lock.json 的成员一一对应，缺项/多项直接报错。
    $lock = Get-Dlss5Lock
    if (-not [IO.File]::Exists($script:ManifestFile)) { throw '缺少 install-manifest.json：安装清单必须先于安装建立。' }
    $manifest = [IO.File]::ReadAllText($script:ManifestFile, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json
    if ($manifest.schema -ne 1 -or @($manifest.entries).Count -lt 1) { throw '安装清单格式不受支持。' }
    $problems = [Collections.Generic.List[string]]::new()
    $entries = @{}
    foreach ($entry in $manifest.entries) {
        $target = [string]$entry.target
        if ($entries.ContainsKey($target)) { $problems.Add("清单重复目标：$target") | Out-Null; continue }
        $entries[$target] = $entry
        $member = @($lock.members | Where-Object { [string]$_.target -eq $target })
        if ($member.Count -ne 1) { $problems.Add("清单目标不在组件锁中：$target") | Out-Null; continue }
        if ([string](Get-ManifestField $entry "source_kind") -ne [string]$member[0].source_kind) { $problems.Add("来源类别与组件锁不一致：$target") | Out-Null }
        if ([bool](Get-ManifestField $entry "replaces_original") -ne [bool]$member[0].replaces_original) { $problems.Add("原件替换标记与组件锁不一致：$target") | Out-Null }
        if (-not (Get-ManifestField $entry "folder") -or -not (Get-ManifestField $entry "purpose")) { $problems.Add("清单条目缺少目录或用途：$target") | Out-Null }
    }
    foreach ($member in $lock.members) {
        if (-not $entries.ContainsKey([string]$member.target)) { $problems.Add("组件锁成员未列入安装清单：$($member.target)") | Out-Null }
    }
    if ($problems.Count) { throw ('安装清单与组件锁不一致：' + ((@($problems) | Select-Object -Unique) -join '；')) }
    return [pscustomobject]@{ Version = [string]$manifest.updated; Entries = $entries;
        Order = @($manifest.entries); Residue = @($manifest.residue) }
}

function Get-Dlss5ClientRecordPath([string]$Client) {
    return Get-MemberPath $Client 'host64/dlss5-install-record.json'
}

function Test-Dlss5ClientRecord([string]$Client) {
    # zh: 客户端内安装记录——放在客户端里而不是只写 _scratch，清理草稿区或换机后仍可卸载。
    $path = Get-Dlss5ClientRecordPath $Client
    if (-not [IO.File]::Exists($path)) { return $null }
    try {
        $record = [IO.File]::ReadAllText($path, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json
        if ($record.schema -ne 1 -or -not $record.entries) { return $null }
        return [pscustomobject]@{ Path = $path; Record = $record;
            BackupRoot = (Get-MemberPath $Client 'host64/dlss5-backup') }
    } catch { return $null }
}

function Export-Dlss5ManifestList {
    # zh: 把安装清单渲染成「要往客户端目录移入什么」的清单文件，供用户核对或随包分发。
    [CmdletBinding()]
    param([string]$Client, [string]$Destination)
    $manifest = Get-Dlss5Manifest
    $lock = Get-Dlss5Lock
    $clientFull = if ($Client) { Get-CanonicalPath $Client } else { $null }
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add('# WOW-335-NR 客户端安装清单')
    $lines.Add('')
    $lines.Add('本清单先于安装建立并随组件维护；实际安装按本清单逐项执行、逐项回读。目标路径相对客户端根目录。')
    $lines.Add('安装时逐项记录安装前状态：不存在的记为「新建」，存在的先备份再替换；卸载按该记录恢复或删除。')
    $lines.Add('')
    if ($clientFull) {
        $lines.Add('当前比对客户端：' + $clientFull)
        $lines.Add('')
        $lines.Add('| 状态 | 目标 | 目录 | 来源 | 必需 | 用途 |')
        $lines.Add('| --- | --- | --- | --- | --- | --- |')
    } else {
        $lines.Add('| 目标 | 目录 | 来源 | 必需 | 用途 |')
        $lines.Add('| --- | --- | --- | --- | --- |')
    }
    foreach ($member in $lock.members) {
        $target = [string]$member.target
        $entry = $manifest.Entries[$target]
        $originValue = Get-ManifestField $entry "origin"
        $origin = if ($originValue) { [string]$originValue } else { [string]$member.source_kind }
        $requiredValue = Get-ManifestField $entry "required"
        $required = if ($null -ne $requiredValue) { if ([bool]$requiredValue) { '必需' } else { '可选' } } else { '必需' }
        $purposeValue = Get-ManifestField $entry "purpose"
        $purpose = if ($purposeValue) { [string]$purposeValue } else { '' }
        $folderValue = Get-ManifestField $entry "folder"
        $folder = if ($folderValue) { [string]$folderValue } else { '' }
        if ($clientFull) {
            $path = Get-MemberPath $clientFull $target
            $state = if ([IO.Directory]::Exists($path)) { '目录冲突' }
                     elseif (-not [IO.File]::Exists($path)) { '待移入' }
                     elseif ((Get-Sha256 $path) -eq [string]$member.sha256) { '已就位' }
                     else { '已存在（内容不同）' }
            $lines.Add("| $state | ``$target`` | $folder | $origin | $required | $purpose |")
        } else {
            $lines.Add("| ``$target`` | $folder | $origin | $required | $purpose |")
        }
    }
    $lines.Add('')
    $lines.Add('## 卸载时会删除的运行产物')
    $lines.Add('')
    foreach ($item in $manifest.Residue) {
        $action = if ([string]$item.policy -eq 'delete') { '删除' } else { '只报告，不删除' }
        $lines.Add(('- `{0}`（{1}）：{2}' -f $item.path, $action, $item.note))
    }
    $lines.Add('')
    $lines.Add('安装器另在客户端写入 `host64\dlss5-install-record.json` 与 `host64\dlss5-backup\`；卸载完成后一并清除。')
    $body = ($lines -join "`r`n") + "`r`n"
    if (-not $Destination) { return $body }
    $target = Get-CanonicalPath $Destination
    if ([IO.File]::Exists($target) -or [IO.Directory]::Exists($target)) { throw '输出文件已存在。' }
    Assert-NoReparse $target
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($body)
    $stream = [IO.File]::Open($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
    return $target
}

function Assert-Client([string]$Client, $Lock, [bool]$ScratchOnly = $false) {
    $clientFull = Get-CanonicalPath $Client
    if ($ScratchOnly -and (-not (Test-Within $clientFull $script:ScratchRoot) -or
            $clientFull.Equals($script:ScratchRoot, [StringComparison]::OrdinalIgnoreCase))) {
        throw '当前操作只允许 _scratch 中的隔离客户端。'
    }
    Assert-NoReparse $clientFull
    foreach ($root in $script:ProtectedRoots) {
        if (Test-Within $clientFull $root) { throw '拒绝操作原版分发基准或只读参考目录。' }
    }
    $exe = Get-MemberPath $clientFull 'wow.exe'
    if (-not [IO.File]::Exists($exe)) { throw '所选目录下没有 wow.exe。' }
    # zh: 不比对 wow.exe 哈希（20261112）——存在官方与第三方修改过的客户端。
    # 只确认是 32 位程序，避免把 64 位客户端当成 WoW 3.3.5a 装进去。
    $machine = Get-PeMachine $exe
    if ($machine -ne 'x86') {
        throw "wow.exe 不是 32 位程序（检测到：$machine）。请选择 WoW 3.3.5a 32 位客户端根目录。"
    }
    return $clientFull
}

function Assert-ProcessesStopped {
    $names = @('wow.exe', 'wowclassic.exe', 'patchmenu.exe', 'AIO DLSS5 32-bit Wrapper.exe')
    $running = @(Get-CimInstance Win32_Process -ErrorAction Stop | Where-Object { $names -contains $_.Name })
    if ($running.Count -gt 0) { throw '请先退出游戏、启动器和 AIO 载体。' }
}

function Get-Dlss5InstallPreview {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Client, [Parameter(Mandatory)][string]$Stage)
    $lock = Get-Dlss5Lock
    $manifest = Get-Dlss5Manifest
    $clientFull = Assert-Client $Client $lock
    $stageFull = Get-CanonicalPath $Stage
    if (-not [IO.Directory]::Exists($stageFull)) { throw '组件来源目录不存在。' }
    Assert-NoReparse $stageFull
    if (Test-Within $script:OperationsRoot $clientFull) { throw '备份目录不能位于客户端内。' }
    $items = @()
    foreach ($member in $lock.members) {
        $targetName = [string]$member.target
        $entry = $manifest.Entries[$targetName]
        $requiredValue = Get-ManifestField $entry "required"
        $required = if ($null -ne $requiredValue) { [bool]$requiredValue } else { $true }
        $root = if ($member.source_kind -in @('stage', 'runtime')) { $stageFull } else { Join-Path $script:ModuleRoot 'templates' }
        $source = Get-MemberPath $root ([string]$member.source)
        $target = Get-MemberPath $clientFull $targetName
        $issue = $null
        $preState = 'absent'
        $existingSha = $null
        if (-not [IO.File]::Exists($source)) {
            if ($required) { $issue = '来源文件缺失' } else { $preState = 'missing-optional' }
        } elseif ((Get-Sha256 $source) -ne [string]$member.sha256) {
            $issue = '来源文件哈希与组件锁不一致'
        } elseif ([IO.Directory]::Exists($target)) {
            $issue = '目标位置是目录'
        } elseif ([IO.File]::Exists($target)) {
            $existingSha = Get-Sha256 $target
            if ($existingSha -eq [string]$member.sha256) { $preState = 'same' }
            elseif ([bool]$member.replaces_original) { $preState = 'present' }
            else { $issue = '目标文件已存在且内容不同' }
        }
        $items += [pscustomobject]@{
            Target = $targetName; Source = $source; TargetPath = $target;
            Sha256 = [string]$member.sha256; ReplacesOriginal = [bool]$member.replaces_original;
            PreState = $preState; ExistingSha256 = $existingSha; Issue = $issue; Required = $required;
            Folder = [string](Get-ManifestField $entry 'folder');
            Label = if (Get-ManifestField $entry 'label') { [string](Get-ManifestField $entry 'label') } else { $targetName };
            Origin = if (Get-ManifestField $entry 'origin') { [string](Get-ManifestField $entry 'origin') } else { [string]$member.source_kind };
            Purpose = [string](Get-ManifestField $entry 'purpose');
            UserAction = [string](Get-ManifestField $entry 'user_action');
            Note = [string](Get-ManifestField $entry 'note')
        }
    }
    return [pscustomobject]@{ Client = $clientFull; Stage = $stageFull; Items = $items;
        ManifestVersion = $manifest.Version;
        CanInstall = @($items | Where-Object { $null -ne $_.Issue }).Count -eq 0;
        Replaced = @($items | Where-Object { $_.PreState -eq 'present' }).Count;
        NewFiles = @($items | Where-Object { $_.PreState -eq 'absent' }).Count;
        SameFiles = @($items | Where-Object { $_.PreState -eq 'same' }).Count;
        SkippedOptional = @($items | Where-Object { $_.PreState -eq 'missing-optional' }).Count }
}

function Write-JsonAtomic([string]$Path, $Value) {
    $temp = "$Path.new-$([guid]::NewGuid().ToString('N'))"
    $json = ($Value | ConvertTo-Json -Depth 20) + "`n"
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json)
    $stream = [IO.File]::Open($temp, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) } finally { $stream.Dispose() }
    if ([IO.File]::Exists($Path)) { Move-AtomicReplace $temp $Path }
    else { [IO.File]::Move($temp, $Path) }
}

function Copy-Verified([string]$Source, [string]$Target, [string]$Expected) {
    if ([IO.File]::Exists($Target) -or [IO.Directory]::Exists($Target)) { throw "暂存目标已存在：$Target" }
    Assert-NoReparse $Source
    Assert-NoReparse $Target
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($Target)) | Out-Null
    Assert-NoReparse $Target
    $reader = $null
    $writer = $null
    $created = $false
    try {
        $reader = [IO.File]::Open($Source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
        $writer = [IO.File]::Open($Target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $created = $true
        $reader.CopyTo($writer)
        $writer.Flush($true)
        $writer.Dispose(); $writer = $null
        $reader.Dispose(); $reader = $null
        if ((Get-Sha256 $Target) -ne $Expected) { throw "文件回读不一致：$Target" }
    } catch {
        if ($null -ne $writer) { $writer.Dispose(); $writer = $null }
        if ($null -ne $reader) { $reader.Dispose(); $reader = $null }
        if ($created -and [IO.File]::Exists($Target)) { [IO.File]::Delete($Target) }
        throw
    } finally {
        if ($null -ne $writer) { $writer.Dispose() }
        if ($null -ne $reader) { $reader.Dispose() }
    }
}

function Invoke-Dlss5Install {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Client, [Parameter(Mandatory)][string]$Stage)
    Assert-ProcessesStopped
    $preview = Get-Dlss5InstallPreview -Client $Client -Stage $Stage
    if (-not $preview.CanInstall) { throw '安装预检未通过；请查看文件冲突和来源状态。' }
    $items = @($preview.Items | Where-Object { $_.PreState -notin @('same', 'missing-optional') })
    if ($items.Count -eq 0) {
        return [pscustomobject]@{ Status = 'already_current'; Operation = $null; Count = 0;
            Client = $preview.Client; Record = (Get-Dlss5ClientRecordPath $preview.Client) }
    }
    $manifest = Get-Dlss5Manifest
    Assert-NoReparse $script:OperationsRoot
    [IO.Directory]::CreateDirectory($script:OperationsRoot) | Out-Null
    $operation = Join-Path $script:OperationsRoot ([guid]::NewGuid().ToString('N'))
    if (Test-Path -LiteralPath $operation) { throw '事务目录已存在。' }
    [IO.Directory]::CreateDirectory($operation) | Out-Null
    Assert-NoReparse $operation
    $recordPath = Join-Path $operation 'transaction.json'
    # zh: 原件备份也放客户端内（host64\dlss5-backup）——
    # 卸载时即使草稿区已清理、换机，也能按客户端内记录还原（20261112）。
    $backupRoot = Get-MemberPath $preview.Client 'host64/dlss5-backup'
    [IO.Directory]::CreateDirectory($backupRoot) | Out-Null
    Assert-NoReparse $backupRoot
    $entries = [Collections.Generic.List[object]]::new()
    foreach ($item in $items) {
        $backupRel = $null
        if ($item.PreState -eq 'present') {
            $backupRel = [string]$item.Target
            $backupPath = Get-MemberPath $backupRoot $backupRel
            Copy-Verified $item.TargetPath $backupPath ([string]$item.ExistingSha256)
            Copy-Verified $item.TargetPath (Get-MemberPath $operation ('backup/' + $backupRel)) ([string]$item.ExistingSha256)
        }
        $entries.Add([ordered]@{ target = [string]$item.Target; sha256 = [string]$item.Sha256;
            replaces_original = [bool]$item.ReplacesOriginal; pre_state = [string]$item.PreState;
            previous_sha256 = $item.ExistingSha256; backup = $backupRel })
    }
    $record = [ordered]@{ schema = 2; status = 'preparing'; client = $preview.Client; stage = $preview.Stage;
        manifest_version = $preview.ManifestVersion; tool = 'WOW-335-NR';
        entries = @($entries); applied = @(); errors = @() }
    Write-JsonAtomic $recordPath $record
    $changed = [Collections.Generic.List[object]]::new()
    $pendingTemps = [Collections.Generic.List[object]]::new()
    try {
        foreach ($item in $items) {
            $staged = Get-MemberPath $operation ('stage/' + $item.Target)
            Copy-Verified $item.Source $staged ([string]$item.Sha256)
        }
        Write-Dlss5CoreDebug ("staged {0} members; entering install phase" -f $items.Count)
        $record.status = 'in_progress'
        Write-JsonAtomic $recordPath $record
        Assert-ProcessesStopped
        foreach ($item in $items) {
            Assert-ProcessesStopped
            $dest = Get-MemberPath $preview.Client ([string]$item.Target)
            $src = Get-MemberPath $operation ('stage/' + [string]$item.Target)
            if ((Get-Sha256 $src) -ne [string]$item.Sha256) { throw "暂存文件身份变化：$($item.Target)" }
            if ([IO.Directory]::Exists($dest)) { throw "目标位置是目录：$($item.Target)" }
            if ([IO.File]::Exists($dest)) {
                if ((Get-Sha256 $dest) -ne [string]$item.ExistingSha256) { throw "目标文件在安装前被改动：$($item.Target)" }
            }
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dest)) | Out-Null
            Assert-NoReparse $dest
            $temp = "$dest.dlss5-installing"
            if ([IO.File]::Exists($temp) -or [IO.Directory]::Exists($temp)) { throw "暂存路径已存在：$temp" }
            try { Copy-Verified $src $temp ([string]$item.Sha256) }
            catch {
                if ([IO.File]::Exists($temp) -or [IO.Directory]::Exists($temp)) {
                    $record.errors = @($record.errors) + @("暂存文件需要检查：$temp")
                }
                throw
            }
            $pendingTemps.Add([pscustomobject]@{ Path = $temp; Sha256 = [string]$item.Sha256 })
            if ([IO.File]::Exists($dest)) { Move-AtomicReplace $temp $dest } else { [IO.File]::Move($temp, $dest) }
            $changed.Add($item)
            if ((Get-Sha256 $dest) -ne [string]$item.Sha256) { throw "安装回读不一致：$($item.Target)" }
            $record.applied = @($record.applied) + @([string]$item.Target)
            Write-JsonAtomic $recordPath $record
        }
        $record.status = 'completed'
        Write-JsonAtomic $recordPath $record
        $clientRecord = [ordered]@{ schema = 1; status = 'installed';
            installed_utc = [DateTime]::UtcNow.ToString('o'); client = $preview.Client;
            manifest_version = $preview.ManifestVersion; tool = 'WOW-335-NR'; operation = $operation;
            entries = @($entries); residue = @($manifest.Residue) }
        Write-JsonAtomic (Get-Dlss5ClientRecordPath $preview.Client) $clientRecord
        Write-Dlss5CoreDebug ("install completed: {0} members, operation {1}" -f $changed.Count, $operation)
        return [pscustomobject]@{ Status = 'completed'; Operation = $operation; Count = $changed.Count;
            Client = $preview.Client; Record = (Get-Dlss5ClientRecordPath $preview.Client) }
    } catch {
        $cause = $_.Exception.Message
        Write-Dlss5CoreDebug ("install failed, rolling back: {0}" -f $cause)
        foreach ($tempInfo in $pendingTemps) {
            try {
                if ([IO.File]::Exists($tempInfo.Path)) {
                    if ((Get-Sha256 $tempInfo.Path) -ne $tempInfo.Sha256) { throw '暂存文件变化，保留现状。' }
                    [IO.File]::Delete($tempInfo.Path)
                }
            } catch { $record.errors = @($record.errors) + @("暂存文件：$($_.Exception.Message)") }
        }
        $rollbackEntries = @($changed | ForEach-Object { [ordered]@{ target = [string]$_.Target;
            sha256 = [string]$_.Sha256; previous_sha256 = $_.ExistingSha256;
            pre_state = [string]$_.PreState; backup = [string]$_.Target } })
        if ($rollbackEntries.Count) {
            $rollbackReport = Join-Path $operation 'rollback.json'
            $null = Invoke-Dlss5Teardown -ClientFull $preview.Client -Entries $rollbackEntries `
                -BackupRoot $backupRoot -SaveRoot (Join-Path $operation 'rollback-modified') -ReportPath $rollbackReport
            try {
                $saved = [IO.File]::ReadAllText($rollbackReport, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json
                $record.errors = @($record.errors) + @($saved.errors)
            } catch { }
        }
        $record.status = if (@($record.errors).Count) { 'rollback_incomplete' } else { 'rolled_back' }
        $record.errors = @($record.errors) + @("安装错误：$cause")
        Write-JsonAtomic $recordPath $record
        throw "安装未完成（$($record.status)），记录：$recordPath；$cause"
    }
}

function Get-VerifiedOperation([string]$Operation) {
    $lock = Get-Dlss5Lock
    $operationFull = Get-CanonicalPath $Operation
    if (-not (Test-Within $operationFull $script:OperationsRoot) -or
        -not ([IO.Path]::GetDirectoryName($operationFull).Equals($script:OperationsRoot, [StringComparison]::OrdinalIgnoreCase))) {
        throw '事务记录不在受控目录。'
    }
    Assert-NoReparse $operationFull
    $recordPath = Get-MemberPath $operationFull 'transaction.json'
    $record = [IO.File]::ReadAllText($recordPath, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json
    if ($record.schema -notin @(1, 2) -or $record.status -notin @('completed', 'in_progress', 'rollback_incomplete')) {
        throw '安装记录不受支持或已还原。'
    }
    $client = Assert-Client ([string](Get-ManifestField $record 'client')) $lock
    # zh: 不再要求记录成员数等于当前锁成员数（20261112）——锁新增成员（例如 nvngx_dlssg.dll）
    # 不应让此前的安装记录变成「不可卸载」。只要求记录里每一项都能在锁中找到同哈希同类型的成员。
    $entries = @()
    # zh: schema 1 只有 members，schema 2 只有 entries；StrictMode 下访问缺失属性会抛错，
    # 统一用 PSObject.Properties 安全取值（20261112）。
    $rawItems = @()
    foreach ($name in @('members', 'entries')) {
        $property = $record.PSObject.Properties[$name]
        if ($null -ne $property -and $null -ne $property.Value) { $rawItems += @($property.Value) }
    }
    $raw = @($rawItems | Where-Object { $null -ne $_ })
    foreach ($item in $raw) {
        $target = [string]$item.target
        $expected = @($lock.members | Where-Object { [string]$_.target -eq $target })
        if ($expected.Count -ne 1) { throw "安装记录含未知成员：$target" }
        if ([string]$item.sha256 -ne [string]$expected[0].sha256) { throw "安装记录与组件锁哈希不一致：$target" }
        if (@($entries | Where-Object { $_.target -eq $target }).Count -gt 0) { continue }
        $previous = if ($record.schema -eq 2) { $item.previous_sha256 } else { $null }
        $backupRel = if ($record.schema -eq 2) { $item.backup }
                     elseif ([bool]$item.replaces_original) { 'd3d9.dll' } else { $null }
        $preState = if ($record.schema -eq 2) { [string]$item.pre_state }
                    elseif ([bool]$item.replaces_original) { 'present' } else { 'absent' }
        $entries += [ordered]@{ target = $target; sha256 = [string]$item.sha256;
            previous_sha256 = $previous; pre_state = $preState; backup = $backupRel }
    }
    $backupRoot = Join-Path $operationFull 'backup'
    foreach ($entry in $entries) {
        if ($entry.pre_state -ne 'present') { continue }
        if (-not $entry.backup) { throw "安装记录缺少原件备份路径：$($entry.target)" }
        $backup = Get-MemberPath $operationFull ('backup/' + [string]$entry.backup)
        if (-not [IO.File]::Exists($backup)) { throw "原件备份缺失：$($entry.target)" }
        if ($entry.previous_sha256 -and (Get-Sha256 $backup) -ne [string]$entry.previous_sha256) {
            throw "原件备份身份不符：$($entry.target)"
        }
    }
    return [pscustomobject]@{ Directory = $operationFull; RecordPath = $recordPath; Record = $record;
        Client = $client; BackupRoot = $backupRoot; Entries = $entries; Lock = $lock }
}

function Get-Dlss5Operations {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Client)
    $clientFull = Get-CanonicalPath $Client
    if (-not [IO.Directory]::Exists($script:OperationsRoot)) { return @() }
    Assert-NoReparse $script:OperationsRoot
    foreach ($directory in Get-ChildItem -LiteralPath $script:OperationsRoot -Directory -Force) {
        if (($directory.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { continue }
        try {
            $verified = Get-VerifiedOperation $directory.FullName
            if ($verified.Client.Equals($clientFull, [StringComparison]::OrdinalIgnoreCase)) {
                [pscustomobject]@{ Operation = $verified.Directory; Status = $verified.Record.status;
                    Label = "$($directory.Name)  [$($verified.Record.status)]" }
            }
        } catch { continue }
    }
}

function Invoke-Dlss5Restore {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Operation)
    Assert-ProcessesStopped
    $verified = Get-VerifiedOperation $Operation
    $runId = [guid]::NewGuid().ToString('N')
    $reportPath = Get-MemberPath $verified.Directory ("restore-$runId.json")
    $saveRoot = Get-MemberPath $verified.Directory ("modified-$runId")
    $report = Invoke-Dlss5Teardown -ClientFull $verified.Client -Entries @($verified.Entries) `
        -BackupRoot $verified.BackupRoot -SaveRoot $saveRoot -ReportPath $reportPath
    if ($report.status -eq 'rolled_back') {
        $verified.Record.status = 'rolled_back'
        $verified.Record | Add-Member -NotePropertyName restore_record -NotePropertyValue $reportPath -Force
        Write-JsonAtomic $verified.RecordPath $verified.Record
        # zh: 恢复成功后同步清掉客户端内安装记录与原件备份——否则「已恢复」的客户端
        # 仍会被 Get-Dlss5Status 报成「已安装」，卸载也会按过期记录再跑一遍（20261112）。
        try {
            $clientRecord = Get-Dlss5ClientRecordPath $verified.Client
            $clientBackup = Get-MemberPath $verified.Client 'host64/dlss5-backup'
            if ([IO.Directory]::Exists($clientBackup)) { [IO.Directory]::Delete($clientBackup, $true) }
            if ([IO.File]::Exists($clientRecord)) { [IO.File]::Delete($clientRecord) }
        } catch { $report.errors = @($report.errors) + @("清理客户端内安装记录：$($_.Exception.Message)") }
    }
    return [pscustomobject]@{ Status = $report.status; Report = $reportPath;
        SavedModified = @($report.saved_modified).Count; Errors = @($report.errors) }
}

function Invoke-Dlss5Uninstall {
    # zh: 卸载读客户端内的安装记录（host64\dlss5-install-record.json），不依赖 _scratch 事务记录。
    # 按记录逐项：安装前存在 → 还原原件；安装前不存在 → 删除新增文件；然后清运行产物。
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Client, [switch]$KeepRecord)
    Assert-ProcessesStopped
    $lock = Get-Dlss5Lock
    $null = Get-Dlss5Manifest
    $clientFull = Assert-Client $Client $lock
    $recordInfo = Test-Dlss5ClientRecord $clientFull
    if ($null -eq $recordInfo) {
        throw '客户端内没有安装记录（host64\dlss5-install-record.json），无法按清单卸载。若还保留 _scratch 事务记录，可改用「恢复安装前文件」。'
    }
    $runId = [guid]::NewGuid().ToString('N')
    Assert-NoReparse $script:OperationsRoot
    [IO.Directory]::CreateDirectory($script:OperationsRoot) | Out-Null
    $reportPath = Join-Path $script:OperationsRoot ("uninstall-$runId.json")
    $saveRoot = Join-Path $script:OperationsRoot ("uninstall-modified-$runId")
    $entries = @(@($recordInfo.Record.entries) | ForEach-Object { [ordered]@{ target = [string]$_.target;
        sha256 = [string]$_.sha256; previous_sha256 = $_.previous_sha256;
        pre_state = [string]$_.pre_state; backup = $_.backup } })
    $report = Invoke-Dlss5Teardown -ClientFull $clientFull -Entries $entries `
        -BackupRoot $recordInfo.BackupRoot -SaveRoot $saveRoot -ReportPath $reportPath
    $removed = @(); $reported = @()
    if ($report.status -eq 'rolled_back') {
        foreach ($item in @($recordInfo.Record.residue)) {
            $rel = [string](Get-ManifestField $item 'path')
            if (-not $rel) { continue }
            if ([string](Get-ManifestField $item 'policy') -ne 'delete') { $reported += $rel; continue }
            if ($rel -like '%*%') { $reported += $rel; continue }
            try {
                $path = Get-MemberPath $clientFull $rel
                if ([IO.Directory]::Exists($path)) { [IO.Directory]::Delete($path, $true); $removed += $rel }
                elseif ([IO.File]::Exists($path)) { [IO.File]::Delete($path); $removed += $rel }
            } catch { $report.errors = @($report.errors) + @("残留 $rel：$($_.Exception.Message)") }
        }
        try {
            if ([IO.Directory]::Exists($recordInfo.BackupRoot)) { [IO.Directory]::Delete($recordInfo.BackupRoot, $true) }
            if (-not $KeepRecord -and [IO.File]::Exists($recordInfo.Path)) { [IO.File]::Delete($recordInfo.Path) }
        } catch { $report.errors = @($report.errors) + @("清理安装记录：$($_.Exception.Message)") }
    }
    $status = if (@($report.errors).Count) { 'rollback_incomplete' } else { 'uninstalled' }
    $summary = [ordered]@{ status = $status; client = $clientFull; restored = @($report.restored);
        deleted = @($report.deleted); saved_modified = @($report.saved_modified);
        residue_removed = @($removed); residue_reported = @($reported); errors = @($report.errors); report = $reportPath }
    Write-JsonAtomic $reportPath $summary
    return [pscustomobject]@{ Status = $status; Report = $reportPath;
        Restored = @($report.restored).Count; Deleted = @($report.deleted).Count;
        SavedModified = @($report.saved_modified).Count; ResidueRemoved = @($removed).Count;
        ResidueReported = @($reported); Errors = @($report.errors) }
}

function Invoke-Dlss5Repair {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Operation)
    Assert-ProcessesStopped
    $verified = Get-VerifiedOperation $Operation
    if ($verified.Record.status -ne 'completed') { throw '未完成的安装请先恢复，不能直接修复。' }
    $runId = [guid]::NewGuid().ToString('N')
    $reportPath = Get-MemberPath $verified.Directory ("repair-$runId.json")
    $conflictRoot = Get-MemberPath $verified.Directory ("repair-modified-$runId")
    $report = [ordered]@{ status = 'in_progress'; repaired = @(); preserved = @(); errors = @() }
    # zh: 可选运行库（nvngx_dlssg.dll）未在本次安装中时，暂存里本来就没有它；
    # 修复跳过这类成员，不再整体报错（20261112）。
    Write-JsonAtomic $reportPath $report
    foreach ($member in $verified.Lock.members) {
        try {
            Assert-ProcessesStopped
            $dest = Get-MemberPath $verified.Client ([string]$member.target)
            if ([IO.Directory]::Exists($dest)) { throw '目标位置是目录，保留现状。' }
            $exists = [IO.File]::Exists($dest)
            $current = if ($exists) { Get-Sha256 $dest } else { $null }
            if ($current -eq $member.sha256) { continue }
            if ($exists -and $member.target -in @('host64/ReShade.ini', 'dlss5-aio-x86.cfg', 'ReShade.ini', 'host64/ReShadePreset.ini')) {
                $report.preserved = @($report.preserved) + @([string]$member.target)
                continue
            }
            if ($exists) {
                $saved = Get-MemberPath $conflictRoot ([string]$member.target)
                Copy-Verified $dest $saved $current
            }
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dest)) | Out-Null
            Assert-NoReparse $dest
            $temp = "$dest.dlss5-repair-$runId"
            $staged = Get-MemberPath $verified.Directory ('stage/' + $member.target)
            if (-not [IO.File]::Exists($staged) -or (Get-Sha256 $staged) -ne $member.sha256) { continue }
            Copy-Verified $staged $temp ([string]$member.sha256)
            if ($exists) {
                if ((Get-Sha256 $dest) -ne $current) { throw '文件在修复期间变化，保留现状。' }
                Move-AtomicReplace $temp $dest
            } else { [IO.File]::Move($temp, $dest) }
            if ((Get-Sha256 $dest) -ne $member.sha256) { throw '修复回读失败。' }
            $report.repaired = @($report.repaired) + @([string]$member.target)
        } catch { $report.errors = @($report.errors) + @("$($member.target)：$($_.Exception.Message)") }
    }
    $report.status = if (@($report.errors).Count) { 'repair_incomplete' } else { 'completed' }
    Write-JsonAtomic $reportPath $report
    return [pscustomobject]@{ Status = $report.status; Report = $reportPath; Repaired = @($report.repaired).Count;
        Preserved = @($report.preserved); Errors = @($report.errors) }
}

function Find-IniSection([string]$Text, [string]$Section) {
    $headers = [regex]::Matches($Text, '(?m)^[\t ]*\[([^\]\r\n]+)\][\t ]*(?:\r?\n|$)')
    $found = @()
    for ($i = 0; $i -lt $headers.Count; $i++) {
        if ($headers[$i].Groups[1].Value.Trim().Equals($Section, [StringComparison]::OrdinalIgnoreCase)) {
            $start = $headers[$i].Index + $headers[$i].Length
            $end = if ($i + 1 -lt $headers.Count) { $headers[$i + 1].Index } else { $Text.Length }
            $found += [pscustomobject]@{ Start = $start; End = $end }
        }
    }
    if ($found.Count -ne 1) { throw "配置节不存在或重复：$Section" }
    return $found[0]
}

function Get-IniValue([string]$Text, [string]$Section, [string]$Key) {
    $range = Find-IniSection $Text $Section
    $segment = $Text.Substring($range.Start, $range.End - $range.Start)
    $pattern = '(?mi)^[\t ]*' + [regex]::Escape($Key) + '[\t ]*=[\t ]*([^\r\n]*)(?=\r?$)'
    $matches = [regex]::Matches($segment, $pattern)
    if ($matches.Count -gt 1) { throw "配置键重复：$Key" }
    if ($matches.Count -eq 0) { return $null }
    return $matches[0].Groups[1].Value.Trim()
}

function Set-IniValue([string]$Text, [string]$Section, [string]$Key, [string]$Value) {
    $range = Find-IniSection $Text $Section
    $segment = $Text.Substring($range.Start, $range.End - $range.Start)
    $pattern = '(?mi)^([\t ]*' + [regex]::Escape($Key) + '[\t ]*=[\t ]*)([^\r\n]*)(?=\r?$)'
    $matches = [regex]::Matches($segment, $pattern)
    if ($matches.Count -gt 1) { throw "配置键重复：$Key" }
    if ($matches.Count -eq 1) {
        $group = $matches[0].Groups[2]
        $segment = $segment.Substring(0, $group.Index) + $Value + $segment.Substring($group.Index + $group.Length)
    } else {
        $newline = if ($Text.Contains("`r`n")) { "`r`n" } else { "`n" }
        if ($segment.Length -gt 0 -and -not $segment.EndsWith("`n")) { $segment += $newline }
        $segment += "$Key=$Value$newline"
    }
    return $Text.Substring(0, $range.Start) + $segment + $Text.Substring($range.End)
}

function Read-TextFile([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $bom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xef -and $bytes[1] -eq 0xbb -and $bytes[2] -eq 0xbf
    $offset = if ($bom) { 3 } else { 0 }
    $text = [Text.UTF8Encoding]::new($false, $true).GetString($bytes, $offset, $bytes.Length - $offset)
    return [pscustomobject]@{ Bytes = $bytes; Text = $text; Bom = $bom }
}

function Write-ControlledText([string]$path, $prior, [string]$updated) {
    $encoded = [Text.UTF8Encoding]::new($false).GetBytes($updated)
    if ($prior.Bom) { $encoded = [byte[]](@(0xef, 0xbb, 0xbf) + $encoded) }
    if ([Convert]::ToBase64String($encoded) -eq [Convert]::ToBase64String($prior.Bytes)) {
        return [pscustomobject]@{ Status = 'unchanged'; Backup = $null }
    }
    Assert-NoReparse $script:OperationsRoot
    [IO.Directory]::CreateDirectory($script:OperationsRoot) | Out-Null
    $operation = Join-Path $script:OperationsRoot ('config-' + [guid]::NewGuid().ToString('N'))
    if (Test-Path -LiteralPath $operation) { throw '配置备份目录已存在。' }
    [IO.Directory]::CreateDirectory($operation) | Out-Null
    Assert-NoReparse $operation
    $backup = Get-MemberPath $operation 'before.ini'
    $beforeHash = Get-Sha256 $path
    Copy-Verified $path $backup $beforeHash
    if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($backup)) -ne [Convert]::ToBase64String($prior.Bytes)) {
        throw '配置在备份前发生变化。'
    }
    $temp = "$path.dlss5-settings-$([guid]::NewGuid().ToString('N'))"
    $created = $false
    try {
        $stream = [IO.File]::Open($temp, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $created = $true
        try { $stream.Write($encoded, 0, $encoded.Length); $stream.Flush($true) } finally { $stream.Dispose() }
        Assert-ProcessesStopped
        if ((Get-Sha256 $path) -ne $beforeHash) { throw '配置在保存期间变化，保留现状。' }
        Move-AtomicReplace $temp $path
        $created = $false
        if ([Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) -ne [Convert]::ToBase64String($encoded)) {
            throw '配置回读不一致；备份已保留。'
        }
    } finally {
        if ($created -and [IO.File]::Exists($temp)) { [IO.File]::Delete($temp) }
    }
    return [pscustomobject]@{ Status = 'saved_restart_required'; Backup = $backup }
}

function Set-Dlss5Settings {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Client, [Parameter(Mandatory)][bool]$NeuralRendering,
          [Parameter(Mandatory)][double]$Intensity, [Parameter(Mandatory)][int]$Model,
          [Parameter(Mandatory)][bool]$DetachedPresentation, [Parameter(Mandatory)][bool]$OpticalFlow)
    if ([double]::IsNaN($Intensity) -or [double]::IsInfinity($Intensity) -or $Intensity -lt 0 -or $Intensity -gt 1) {
        throw 'NR 强度必须在 0 到 1 之间。'
    }
    if ($Model -lt 1 -or $Model -gt 3) { throw 'NR 模型必须为 1、2 或 3。' }
    Assert-ProcessesStopped
    $lock = Get-Dlss5Lock
    $clientFull = Assert-Client $Client $lock
    $path = Get-MemberPath $clientFull 'host64/ReShade.ini'
    if (-not [IO.File]::Exists($path)) { throw '缺少 host64/ReShade.ini。' }
    $prior = Read-TextFile $path
    $updated = Set-IniValue $prior.Text 'Standalone.DLSSNR' 'NeuralRendering' $(if ($NeuralRendering) { '1' } else { '0' })
    $updated = Set-IniValue $updated 'Standalone.DLSSNR' 'Model' ([string]$Model)
    $value = $Intensity.ToString('0.##', [Globalization.CultureInfo]::InvariantCulture)
    $updated = Set-IniValue $updated 'Standalone.DLSSNR' 'Intensity' $value
    $updated = Set-IniValue $updated 'Standalone.DLSSNR' 'NvidiaOpticalFlowMotion' $(if ($OpticalFlow) { '1' } else { '0' })
    $updated = Set-IniValue $updated 'Standalone.DLSSNR' 'DetachedPresentation' $(if ($DetachedPresentation) { '1' } else { '0' })
    if ($DetachedPresentation) {
        $updated = Set-IniValue $updated 'Standalone.DLSSNR' 'WindowedInputScaling' '1'
        $updated = Set-IniValue $updated 'Standalone.DLSSNR' 'WindowedVirtualization' '1'
    }
    return Write-ControlledText $path $prior $updated
}

function Set-Dlss5Pipeline {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Client, [Parameter(Mandatory)][bool]$Enabled)
    Assert-ProcessesStopped
    $lock = Get-Dlss5Lock
    $clientFull = Assert-Client $Client $lock
    $path = Get-MemberPath $clientFull 'dlss5-aio-x86.cfg'
    if (-not [IO.File]::Exists($path)) { throw '缺少 dlss5-aio-x86.cfg。' }
    $prior = Read-TextFile $path
    $pattern = '(?mi)^([\t ]*enabled[\t ]*=[\t ]*)([^\r\n]*)(?=\r?$)'
    $matches = [regex]::Matches($prior.Text, $pattern)
    if ($matches.Count -ne 1 -or $matches[0].Groups[2].Value.Trim() -notin @('0', '1')) {
        throw 'AIO 管线开关缺失、重复或数值无效。'
    }
    $group = $matches[0].Groups[2]
    $value = if ($Enabled) { '1' } else { '0' }
    $updated = $prior.Text.Substring(0, $group.Index) + $value + $prior.Text.Substring($group.Index + $group.Length)
    return Write-ControlledText $path $prior $updated
}

function Read-AioTelemetry([int]$ProcessId) {
    $mapping = $null
    $view = $null
    try {
        $mapping = [IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting(
            "Local\DLSS5_AIO_Telemetry_$ProcessId", [IO.MemoryMappedFiles.MemoryMappedFileRights]::Read)
        $view = $mapping.CreateViewAccessor(0, 320, [IO.MemoryMappedFiles.MemoryMappedFileAccess]::Read)
        $bytes = [byte[]]::new(320)
        for ($i = 0; $i -lt 20; $i++) {
            $before = $view.ReadInt64(16)
            if (($before -band 1) -ne 0) { Start-Sleep -Milliseconds 1; continue }
            $null = $view.ReadArray(0, $bytes, 0, $bytes.Length)
            $after = $view.ReadInt64(16)
            if ($before -ne $after -or ($after -band 1) -ne 0 -or
                [BitConverter]::ToInt64($bytes, 16) -ne $after) { continue }
            if ([BitConverter]::ToUInt32($bytes, 0) -ne 0x4f494144 -or
                [BitConverter]::ToUInt32($bytes, 4) -ne 1 -or [BitConverter]::ToUInt32($bytes, 8) -lt 320 -or
                [BitConverter]::ToUInt32($bytes, 12) -ne $ProcessId) { throw '遥测身份或版本不符。' }
            $timestamp = [BitConverter]::ToInt64($bytes, 24)
            $frequency = [BitConverter]::ToInt64($bytes, 32)
            if ($timestamp -le 0 -or $frequency -le 0) { throw '遥测时钟无效。' }
            $age = [Diagnostics.Stopwatch]::GetTimestamp() / [double][Diagnostics.Stopwatch]::Frequency -
                   $timestamp / [double]$frequency
            if ($age -lt -0.1 -or $age -gt 3.0) { throw '遥测未在最近三秒更新。' }
            $flags = [BitConverter]::ToUInt32($bytes, 48)
            return [pscustomobject]@{ Available = $true; AgeSeconds = [Math]::Round($age, 3);
                NrEnabled = [bool]($flags -band 2);
                PipelineReady = [bool]($flags -band 8); ProcessedOutputSelected = [bool]($flags -band 32);
                FrameGeneration = [bool]($flags -band 4); ProxyHidden = [bool]($flags -band 256);
                NrFrames = [BitConverter]::ToUInt64($bytes, 200); SrFrames = [BitConverter]::ToUInt64($bytes, 208) }
        }
        throw '无法取得稳定的遥测快照。'
    } catch {
        return [pscustomobject]@{ Available = $false; Reason = $_.Exception.Message }
    } finally {
        if ($null -ne $view) { $view.Dispose() }
        if ($null -ne $mapping) { $mapping.Dispose() }
    }
}

function Get-PeMachine([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $reader = [IO.BinaryReader]::new($stream)
        if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) { return '未知' }
        $stream.Position = 0x3c
        $offset = $reader.ReadUInt32()
        if ($offset -gt $stream.Length - 6) { return '未知' }
        $stream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x00004550) { return '未知' }
        switch ($reader.ReadUInt16()) {
            0x014c { return 'x86' }
            0x8664 { return 'x64' }
            default { return '未知' }
        }
    } finally { $stream.Dispose() }
}

function Get-Dlss5Status {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Client)
    $lock = Get-Dlss5Lock
    $clientFull = Get-CanonicalPath $Client
    Assert-NoReparse $clientFull
    $files = @()
    $architectures = @{
        'd3d9.dll' = 'x86'; 'standalone-dlssnr.addon32' = 'x86';
        'host64/AIO DLSS5 32-bit Wrapper.exe' = 'x64'; 'host64/dxgi.dll' = 'x64';
        'host64/standalone-dlssnr.addon64' = 'x64'; 'host64/nvngx.dll' = 'x64';
        'host64/nvngx_dlss.dll' = 'x64'; 'host64/nvngx_dlssnr.dll' = 'x64';
        'host64/nvngx_dlssg.dll' = 'x64'
    }
    foreach ($member in $lock.members) {
        $path = Get-MemberPath $clientFull ([string]$member.target)
        $state = if ([IO.Directory]::Exists($path)) { '目录冲突' }
                 elseif (-not [IO.File]::Exists($path)) { '缺失' }
                 elseif ((Get-Sha256 $path) -eq $member.sha256) { '与候选一致' }
                 elseif ($member.target -in @('host64/ReShade.ini', 'dlss5-aio-x86.cfg', 'ReShade.ini')) { '配置已变化' }
                 else { '身份不符' }
        $machine = $null; $version = $null
        if ($architectures.ContainsKey([string]$member.target) -and [IO.File]::Exists($path)) {
            $machine = Get-PeMachine $path
            if ($machine -ne $architectures[[string]$member.target]) { $state = '架构不符' }
            $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($path).FileVersion
        }
        $files += [pscustomobject]@{ Name = [string]$member.target; Status = $state; Machine = $machine; Version = $version }
    }
    $exe = Get-MemberPath $clientFull 'wow.exe'
    # zh: 不再报告「与锁定哈希匹配」——身份口径已改为 存在 + x86 架构（20261112）。
    $gameIdentity = '缺失'
    if ([IO.File]::Exists($exe)) {
        $machine = Get-PeMachine $exe
        $size = (Get-Item -LiteralPath $exe).Length
        $gameIdentity = if ($machine -eq 'x86') { "存在（x86，$size 字节；不比对哈希）" }
                        else { "存在但架构为 $machine（应为 x86）" }
    }
    $configPath = Get-MemberPath $clientFull 'host64/ReShade.ini'
    $config = [ordered]@{ NeuralRendering = $null; Model = $null; Intensity = $null; FrameGeneration = $null;
        NvidiaOpticalFlowMotion = $null; DetachedPresentation = $null; WindowedInputScaling = $null; PipelineEnabled = $null;
        ClassicD3D9CpuBridge = $null; Error = $null }
    if ([IO.File]::Exists($configPath)) {
        try {
            $text = (Read-TextFile $configPath).Text
            foreach ($key in @('NeuralRendering', 'Model', 'Intensity', 'FrameGeneration', 'NvidiaOpticalFlowMotion', 'DetachedPresentation', 'WindowedInputScaling')) {
                $config[$key] = Get-IniValue $text 'Standalone.DLSSNR' $key
            }
        } catch { $config.Error = $_.Exception.Message }
    }
    $bridgePath = Get-MemberPath $clientFull 'dlss5-aio-x86.cfg'
    if ([IO.File]::Exists($bridgePath)) {
        $bridgeText = (Read-TextFile $bridgePath).Text
        $pipelineMatches = [regex]::Matches($bridgeText, '(?mi)^[\t ]*enabled[\t ]*=[\t ]*([^\r\n]*)(?=\r?$)')
        if ($pipelineMatches.Count -eq 1) { $config.PipelineEnabled = $pipelineMatches[0].Groups[1].Value.Trim() }
        elseif ($pipelineMatches.Count -gt 1) { $config.Error = 'AIO 管线开关重复。' }
        $matches = [regex]::Matches($bridgeText, '(?mi)^[\t ]*classic_d3d9_cpu_fallback[\t ]*=[\t ]*([^\r\n]*)(?=\r?$)')
        if ($matches.Count -eq 1) { $config.ClassicD3D9CpuBridge = $matches[0].Groups[1].Value.Trim() }
        elseif ($matches.Count -gt 1) { $config.Error = 'classic_d3d9_cpu_fallback 重复。' }
    }
    $processes = @(Get-CimInstance Win32_Process -ErrorAction Stop | Where-Object {
        $_.Name -in @('wow.exe', 'AIO DLSS5 32-bit Wrapper.exe')
    })
    $gamePath = Get-MemberPath $clientFull 'wow.exe'
    $hostPath = Get-MemberPath $clientFull 'host64/AIO DLSS5 32-bit Wrapper.exe'
    $games = @($processes | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.Equals($gamePath, [StringComparison]::OrdinalIgnoreCase) })
    $hosts = @($processes | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.Equals($hostPath, [StringComparison]::OrdinalIgnoreCase) })
    $unknown = @($processes | Where-Object { -not $_.ExecutablePath }).Count
    $telemetry = if ($hosts.Count -eq 1 -and $unknown -eq 0) { Read-AioTelemetry ([int]$hosts[0].ProcessId) }
                 else { [pscustomobject]@{ Available = $false; Reason = '载体未运行、数量不唯一或身份不可读' } }
    $graphics = @(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object {
        [pscustomobject]@{ Name = $_.Name; DriverVersion = $_.DriverVersion }
    })
    $recordInfo = Test-Dlss5ClientRecord $clientFull
    $installRecord = if ($null -eq $recordInfo) { '没有（首次安装或已卸载）' }
                     else { "存在（$(@($recordInfo.Record.entries).Count) 项，$($recordInfo.Record.installed_utc)）" }
    return [pscustomobject]@{ GameIdentity = $gameIdentity; Files = $files; Config = [pscustomobject]$config;
        InstallRecord = $installRecord;
        GameProcesses = $games.Count; HostProcesses = $hosts.Count; UnidentifiedProcesses = $unknown;
        GraphicsDevices = $graphics; Telemetry = $telemetry }
}

function Export-Dlss5Diagnostics {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Client, [Parameter(Mandatory)][string]$Destination)
    $target = Get-CanonicalPath $Destination
    if ([IO.File]::Exists($target) -or [IO.Directory]::Exists($target)) { throw '诊断文件已存在。' }
    Assert-NoReparse $target
    $status = Get-Dlss5Status -Client $Client
    $report = [ordered]@{ schema = 1; generated_utc = [DateTime]::UtcNow.ToString('o');
        client = '<所选客户端>'; aio_version = '2.2.4'; reshade_version = '6.8.0';
        game_identity = $status.GameIdentity; files = $status.Files; config = $status.Config;
        graphics_devices = $status.GraphicsDevices;
        game_processes = $status.GameProcesses; host_processes = $status.HostProcesses;
        unidentified_processes = $status.UnidentifiedProcesses; telemetry = $status.Telemetry;
        note = '不包含运行时 DLL、账号、聊天、截图或原始日志。' }
    $json = ($report | ConvertTo-Json -Depth 12) + "`n"
    $stream = [IO.File]::Open($target, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $data = [Text.UTF8Encoding]::new($false).GetBytes($json)
        $stream.Write($data, 0, $data.Length)
        $stream.Flush($true)
    } finally { $stream.Dispose() }
    return $target
}

function Start-Dlss5WindowWatch {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Client, [Parameter(Mandatory)][string]$Watcher)
    $lock = Get-Dlss5Lock
    $clientFull = Assert-Client $Client $lock
    $status = Get-Dlss5Status -Client $clientFull
    if ($status.Config.Error -or $status.Config.PipelineEnabled -ne '1' -or
        $status.Config.DetachedPresentation -ne '1' -or $status.Config.WindowedInputScaling -ne '1' -or
        $status.Config.ClassicD3D9CpuBridge -ne '1' -or $status.Config.FrameGeneration -ne '0') {
        throw '当前配置不符合独立输出候选：请检查 AIO 管线、CPU 桥、输入缩放及 FG 状态。'
    }
    $watcherFull = Get-CanonicalPath $Watcher
    $managedWatcher = Get-CanonicalPath (Join-Path $script:ModuleRoot 'bin/window_watch.exe')
    if (-not (Test-Within $watcherFull $script:ScratchRoot) -and
        -not $watcherFull.Equals($managedWatcher, [StringComparison]::OrdinalIgnoreCase)) {
        throw '窗口助手须为受控构建或 _scratch 中的候选。'
    }
    Assert-NoReparse $watcherFull
    $watchLock = [IO.File]::ReadAllText((Join-Path $script:ModuleRoot 'watcher-lock.json'),
        [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json
    if ($watchLock.schema -ne 1 -or -not [IO.File]::Exists($watcherFull) -or
        (Get-Sha256 $watcherFull) -ne $watchLock.files.'window_watch.exe' -or
        (Get-Sha256 (Join-Path $script:ModuleRoot 'native/window_watch.cpp')) -ne $watchLock.files.'window_watch.cpp' -or
        (Get-Sha256 (Join-Path $script:ModuleRoot 'native/window_layer.hpp')) -ne $watchLock.files.'window_layer.hpp') {
        throw '窗口助手或源码身份不符。'
    }
    $running = @(Get-CimInstance Win32_Process -Filter "Name = 'window_watch.exe'" -ErrorAction Stop |
        Where-Object { $_.ExecutablePath -and $_.ExecutablePath.Equals($watcherFull, [StringComparison]::OrdinalIgnoreCase) })
    if ($running.Count -gt 0) { throw '窗口助手候选已在运行。' }
    Assert-NoReparse $script:OperationsRoot
    [IO.Directory]::CreateDirectory($script:OperationsRoot) | Out-Null
    $operation = Join-Path $script:OperationsRoot ('watch-' + [guid]::NewGuid().ToString('N'))
    if (Test-Path -LiteralPath $operation) { throw '助手日志目录已存在。' }
    [IO.Directory]::CreateDirectory($operation) | Out-Null
    Assert-NoReparse $operation
    $game = Get-MemberPath $clientFull 'wow.exe'
    $arguments = @('--game', ('"' + $game + '"'))
    $process = Start-Process -FilePath $watcherFull -ArgumentList $arguments -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $operation 'stdout.log') -RedirectStandardError (Join-Path $operation 'stderr.log')
    return [pscustomobject]@{ ProcessId = $process.Id; LogDirectory = $operation;
        Message = '助手已启动；是否处理成功须查看日志并进行游戏验收。' }
}

$script:AdvancedKeys = @('Model','Intensity','LocalTone','LocalStructure','SkinStructure')
# zh: 参数刻度边界——与 native/tiers.hpp（唯一事实源）、schemes/*.json、模板一致（20261109 收敛）。
$script:NrIntensityMax = 1.0   # 强度/结构 0–1（1.0=满）
$script:NrToneMax = 2.0        # 色调 0–2（1.0=中性）
$script:SchemesRoot = [IO.Path]::GetFullPath((Join-Path $script:ModuleRoot 'schemes'))
$script:UserSchemesRoot = [IO.Path]::GetFullPath((Join-Path $script:ScratchRoot 'dlss5-aio/user-schemes'))

function Get-Dlss5AdvancedSettings {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Client)
    $lock = Get-Dlss5Lock
    $clientFull = Assert-Client $Client $lock
    $path = Get-MemberPath $clientFull 'host64/ReShade.ini'
    if (-not [IO.File]::Exists($path)) { throw '缺少 host64/ReShade.ini。' }
    $text = (Read-TextFile $path).Text
    $defaults = @{ Model='1'; Intensity='1.0'; LocalTone='1.0'; LocalStructure='1.0'; SkinStructure='-1.0' }
    $result = @{}
    foreach ($key in $script:AdvancedKeys) {
        $value = $null
        try { $value = Get-IniValue $text 'Standalone.DLSSNR' $key } catch { $value = $null }
        $result[$key] = if ($value) { $value } else { $defaults[$key] }
    }
    try { $result['NeuralRendering'] = Get-IniValue $text 'Standalone.DLSSNR' 'NeuralRendering' } catch { $result['NeuralRendering'] = $null }
    return [pscustomobject]$result
}

function Set-Dlss5AdvancedSettings {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Client,
          [Parameter(Mandatory)][int]$Model,
          [Parameter(Mandatory)][double]$Intensity,
          [Parameter(Mandatory)][double]$LocalTone,
          [Parameter(Mandatory)][double]$LocalStructure,
          [Parameter(Mandatory)][double]$SkinStructure,
          [bool]$NeuralRendering = $true,
          [Nullable[bool]]$DetachedPresentation = $null,
          [Nullable[bool]]$OpticalFlow = $null)
    if ($Model -lt 1 -or $Model -gt 3) { throw '模型必须为 1、2 或 3。' }
    foreach ($bound in @(@('强度',$Intensity,$script:NrIntensityMax),
                         @('色调结构',$LocalTone,$script:NrToneMax),
                         @('结构强度',$LocalStructure,$script:NrIntensityMax))) {
        $v = [double]$bound[1]
        if ([double]::IsNaN($v) -or [double]::IsInfinity($v) -or $v -lt 0 -or $v -gt $bound[2]) {
            throw ($bound[0] + (' 必须在 0 到 {0} 之间。' -f $bound[2]))
        }
    }
    if ($SkinStructure -lt -1 -or $SkinStructure -gt 1) { throw '皮肤结构必须在 -1 到 1 之间。' }
    Assert-ProcessesStopped
    $lock = Get-Dlss5Lock
    $clientFull = Assert-Client $Client $lock
    $path = Get-MemberPath $clientFull 'host64/ReShade.ini'
    if (-not [IO.File]::Exists($path)) { throw '缺少 host64/ReShade.ini。' }
    $prior = Read-TextFile $path
    $invariant = [Globalization.CultureInfo]::InvariantCulture
    $updated = Set-IniValue $prior.Text 'Standalone.DLSSNR' 'NeuralRendering' $(if ($NeuralRendering) {'1'} else {'0'})
    $updated = Set-IniValue $updated 'Standalone.DLSSNR' 'Model' ([string]$Model)
    $updated = Set-IniValue $updated 'Standalone.DLSSNR' 'Intensity' $Intensity.ToString('0.##', $invariant)
    $updated = Set-IniValue $updated 'Standalone.DLSSNR' 'LocalTone' $LocalTone.ToString('0.##', $invariant)
    $updated = Set-IniValue $updated 'Standalone.DLSSNR' 'LocalStructure' $LocalStructure.ToString('0.##', $invariant)
    $updated = Set-IniValue $updated 'Standalone.DLSSNR' 'SkinStructure' $SkinStructure.ToString('0.##', $invariant)
    if ($null -ne $DetachedPresentation) {
        $updated = Set-IniValue $updated 'Standalone.DLSSNR' 'DetachedPresentation' $(if ($DetachedPresentation) {'1'} else {'0'})
        if ($DetachedPresentation) {
            $updated = Set-IniValue $updated 'Standalone.DLSSNR' 'WindowedInputScaling' '1'
            $updated = Set-IniValue $updated 'Standalone.DLSSNR' 'WindowedVirtualization' '1'
        }
    }
    if ($null -ne $OpticalFlow) {
        $updated = Set-IniValue $updated 'Standalone.DLSSNR' 'NvidiaOpticalFlowMotion' $(if ($OpticalFlow) {'1'} else {'0'})
    }
    return Write-ControlledText $path $prior $updated
}

function Get-Dlss5Schemes {
    $result = @()
    foreach ($entry in @(@('内置', $script:SchemesRoot), @('用户', $script:UserSchemesRoot))) {
        $root = $entry[1]
        if (-not [IO.Directory]::Exists($root)) { continue }
        foreach ($file in (Get-ChildItem -LiteralPath $root -Filter '*.json' -File -ErrorAction SilentlyContinue)) {
            try {
                $scheme = [IO.File]::ReadAllText($file.FullName, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json
                if ($scheme.schema -ne 1 -or -not $scheme.name) { continue }
                $result += [pscustomobject]@{ Name = [string]$scheme.name; Source = $entry[0];
                    Path = $file.FullName; Description = [string]$scheme.description }
            } catch { continue }
        }
    }
    return $result
}

function Save-Dlss5Scheme {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Client, [Parameter(Mandatory)][string]$Name, [string]$Description = '')
    $clean = $Name.Trim()
    if (-not $clean -or $clean.Length -gt 24 -or -1 -ne $clean.IndexOfAny([IO.Path]::GetInvalidFileNameChars())) {
        throw '方案名限 1-24 个字符，且不能含文件名非法字符。'
    }
    $current = Get-Dlss5AdvancedSettings -Client $Client
    $scheme = [ordered]@{ schema = 1; name = $clean; description = $Description;
        created_utc = [DateTime]::UtcNow.ToString('o');
        values = [ordered]@{ neuralRendering = ($current.NeuralRendering -ne '0');
            model = [int]$current.Model; intensity = [double]$current.Intensity;
            localTone = [double]$current.LocalTone; localStructure = [double]$current.LocalStructure;
            skinStructure = [double]$current.SkinStructure } }
    Assert-NoReparse $script:UserSchemesRoot
    [IO.Directory]::CreateDirectory($script:UserSchemesRoot) | Out-Null
    $path = Get-MemberPath $script:UserSchemesRoot ($clean + '.json')
    if ([IO.File]::Exists($path)) { throw '同名方案已存在；请换一个名字。' }
    Write-JsonAtomic $path $scheme
    return $path
}

function Apply-Dlss5Scheme {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Client, [Parameter(Mandatory)][string]$Path)
    $schemePath = Get-CanonicalPath $Path
    if (-not (Test-Within $schemePath $script:ScratchRoot) -and -not (Test-Within $schemePath $script:SchemesRoot)) {
        throw '方案文件必须位于内置目录或 _scratch。'
    }
    Assert-NoReparse $schemePath
    $scheme = [IO.File]::ReadAllText($schemePath, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json
    if ($scheme.schema -ne 1 -or -not $scheme.values) { throw '方案格式不受支持。' }
    $v = $scheme.values
    return Set-Dlss5AdvancedSettings -Client $Client -Model ([int]$v.model) -Intensity ([double]$v.intensity) `
        -LocalTone ([double]$v.localTone) -LocalStructure ([double]$v.localStructure) `
        -SkinStructure ([double]$v.skinStructure) -NeuralRendering ([bool]$v.neuralRendering)
}

function Invoke-Dlss5Teardown {
    # zh: 安装回退与卸载共用的逆向处理核心。按条目逐项：安装前存在 → 还原原件（先留用户改动副本）；
    # 安装前不存在 → 删除新增文件。任何一项失败都不中断其余项，结果按 rolled_back /
    # rollback_incomplete 如实记录，不把失败写成完成（20261112）。
    param([Parameter(Mandatory)][string]$ClientFull, [Parameter(Mandatory)][object[]]$Entries,
          [string]$BackupRoot, [string]$SaveRoot, [string]$ReportPath)
    $report = [ordered]@{ status = 'in_progress'; saved_modified = @(); restored = @(); deleted = @(); errors = @() }
    if ($ReportPath) { Write-JsonAtomic $ReportPath $report }
    $list = @($Entries)
    [array]::Reverse($list)
    foreach ($entry in $list) {
        $target = [string]$entry.target
        try {
            Assert-ProcessesStopped
            $dest = Get-MemberPath $ClientFull $target
            if ([IO.Directory]::Exists($dest)) { throw '目标位置是目录，保留现状。' }
            $exists = [IO.File]::Exists($dest)
            $preState = [string]$entry.pre_state
            if ($preState -eq 'present') {
                $previous = [string]$entry.previous_sha256
                if ($exists -and $previous -and (Get-Sha256 $dest) -eq $previous) {
                    $report.restored = @($report.restored) + @($target); continue
                }
                if (-not $BackupRoot -or -not $entry.backup) { throw '缺少原件备份位置。' }
                $backupPath = Get-MemberPath $BackupRoot ([string]$entry.backup)
                if (-not [IO.File]::Exists($backupPath)) { throw '原件备份缺失。' }
                if ($previous -and (Get-Sha256 $backupPath) -ne $previous) { throw '原件备份身份不符。' }
                if ($exists -and $SaveRoot) {
                    $current = Get-Sha256 $dest
                    $saved = Get-MemberPath $SaveRoot $target
                    Copy-Verified $dest $saved $current
                    $report.saved_modified = @($report.saved_modified) + @($target)
                }
                $temp = "$dest.dlss5-restoring"
                if ([IO.File]::Exists($temp) -or [IO.Directory]::Exists($temp)) { throw "还原暂存路径已存在：$temp" }
                $expect = if ($previous) { $previous } else { Get-Sha256 $backupPath }
                Copy-Verified $backupPath $temp $expect
                [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dest)) | Out-Null
                if ($exists) { Move-AtomicReplace $temp $dest } else { [IO.File]::Move($temp, $dest) }
                if ((Get-Sha256 $dest) -ne $expect) { throw '原件还原回读失败。' }
                $report.restored = @($report.restored) + @($target)
            } else {
                if (-not $exists) { continue }
                if ((Get-Sha256 $dest) -ne [string]$entry.sha256) {
                    if (-not $SaveRoot) { throw '文件安装后被修改，缺少保存位置。' }
                    $current = Get-Sha256 $dest
                    $saved = Get-MemberPath $SaveRoot $target
                    Copy-Verified $dest $saved $current
                    $report.saved_modified = @($report.saved_modified) + @($target)
                }
                [IO.File]::Delete($dest)
                $report.deleted = @($report.deleted) + @($target)
            }
        } catch { $report.errors = @($report.errors) + @("$($target)：$($_.Exception.Message)") }
    }
    $report.status = if (@($report.errors).Count) { 'rollback_incomplete' } else { 'rolled_back' }
    if ($ReportPath) { Write-JsonAtomic $ReportPath $report }
    return [pscustomobject]$report
}

Export-ModuleMember -Function Get-Dlss5InstallPreview, Invoke-Dlss5Install, Invoke-Dlss5Uninstall,
    Get-Dlss5Operations, Invoke-Dlss5Restore, Invoke-Dlss5Repair, Get-Dlss5Status, Set-Dlss5Settings,
    Set-Dlss5Pipeline, Export-Dlss5Diagnostics, Start-Dlss5WindowWatch, Get-Dlss5AdvancedSettings,
    Set-Dlss5AdvancedSettings, Get-Dlss5Schemes, Save-Dlss5Scheme, Apply-Dlss5Scheme,
    Get-Dlss5Manifest, Export-Dlss5ManifestList, Test-Dlss5ClientRecord
