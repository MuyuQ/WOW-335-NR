Set-StrictMode -Version Latest
$script:ModuleRoot = $PSScriptRoot
$script:ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$script:ScratchRoot = [IO.Path]::GetFullPath((Join-Path $script:ProjectRoot '_scratch'))
$script:OperationsRoot = [IO.Path]::GetFullPath((Join-Path $script:ScratchRoot 'dlss5-aio/control-operations'))
$script:LockFile = Join-Path $PSScriptRoot 'payload-lock.json'
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
    if ($lock.schema -ne 1 -or @($lock.members).Count -ne 24) { throw '组件清单格式不受支持。' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($member in $lock.members) {
        if (-not $seen.Add([string]$member.target)) { throw '组件清单中有重复目标。' }
        if ($member.sha256 -notmatch '^[0-9a-f]{64}$' -or $member.source_kind -notin @('stage', 'template')) {
            throw '组件清单包含无效身份。'
        }
        $first = ([string]$member.target -split '[/\\]')[0]
        if ($first -in @('WTF', 'Cache', 'Logs', 'Screenshots', 'Data', 'Interface', 'Fonts')) {
            throw '组件清单触及受保护目录。'
        }
    }
    $replaced = @($lock.members | Where-Object { $_.replaces_original })
    if ($replaced.Count -ne 1 -or $replaced[0].target -ne 'd3d9.dll') { throw '原件替换清单无效。' }
    return $lock
}

function Assert-Client([string]$Client, $Lock, [bool]$ScratchOnly = $true) {
    $clientFull = Get-CanonicalPath $Client
    if ($ScratchOnly -and (-not (Test-Within $clientFull $script:ScratchRoot) -or
            $clientFull.Equals($script:ScratchRoot, [StringComparison]::OrdinalIgnoreCase))) {
        throw '当前候选只允许操作 _scratch 中的隔离客户端。'
    }
    Assert-NoReparse $clientFull
    $exe = Get-MemberPath $clientFull 'wow.exe'
    if (-not [IO.File]::Exists($exe) -or (Get-Sha256 $exe) -ne $Lock.game_sha256) {
        throw 'wow.exe 不存在或身份与锁定版本不符。'
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
    $clientFull = Assert-Client $Client $lock
    $stageFull = Get-CanonicalPath $Stage
    if (-not (Test-Within $stageFull $script:ScratchRoot) -or -not [IO.Directory]::Exists($stageFull)) {
        throw '组件来源须为 _scratch 中已准备的目录。'
    }
    Assert-NoReparse $stageFull
    if (Test-Within $script:OperationsRoot $clientFull) { throw '备份目录不能位于客户端内。' }
    $items = @()
    foreach ($member in $lock.members) {
        $root = if ($member.source_kind -eq 'stage') { $stageFull } else { Join-Path $script:ModuleRoot 'templates' }
        $source = Get-MemberPath $root ([string]$member.source)
        $target = Get-MemberPath $clientFull ([string]$member.target)
        $issue = $null
        if (-not [IO.File]::Exists($source) -or (Get-Sha256 $source) -ne $member.sha256) {
            $issue = '来源文件缺失或哈希不符'
        } elseif ([IO.Directory]::Exists($target)) {
            $issue = '目标位置是目录'
        } elseif ($member.replaces_original) {
            if (-not [IO.File]::Exists($target) -or (Get-Sha256 $target) -ne $lock.original_d3d9_sha256) {
                $issue = '原始 d3d9.dll 缺失或身份不符'
            }
        } elseif ([IO.File]::Exists($target)) {
            $issue = '目标文件已存在'
        }
        $items += [pscustomobject]@{ Target = [string]$member.target; Source = $source; TargetPath = $target;
            Sha256 = [string]$member.sha256; ReplacesOriginal = [bool]$member.replaces_original; Issue = $issue }
    }
    return [pscustomobject]@{ Client = $clientFull; Stage = $stageFull; Items = $items;
        CanInstall = @($items | Where-Object { $null -ne $_.Issue }).Count -eq 0 }
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
    $lock = Get-Dlss5Lock
    Assert-NoReparse $script:OperationsRoot
    [IO.Directory]::CreateDirectory($script:OperationsRoot) | Out-Null
    $operation = Join-Path $script:OperationsRoot ([guid]::NewGuid().ToString('N'))
    if (Test-Path -LiteralPath $operation) { throw '事务目录已存在。' }
    [IO.Directory]::CreateDirectory($operation) | Out-Null
    Assert-NoReparse $operation
    $recordPath = Join-Path $operation 'transaction.json'
    $backup = Get-MemberPath $operation 'backup/d3d9.dll'
    $original = Get-MemberPath $preview.Client 'd3d9.dll'
    $record = [ordered]@{ schema = 1; status = 'preparing'; client = $preview.Client;
        stage = $preview.Stage; backup_sha256 = $lock.original_d3d9_sha256;
        members = @($preview.Items | ForEach-Object { [ordered]@{ target = $_.Target; sha256 = $_.Sha256;
                    replaces_original = $_.ReplacesOriginal } }); applied = @(); errors = @() }
    Write-JsonAtomic $recordPath $record
    $changed = [Collections.Generic.List[object]]::new()
    $pendingTemps = [Collections.Generic.List[object]]::new()
    try {
        Copy-Verified $original $backup $lock.original_d3d9_sha256
        foreach ($item in $preview.Items) {
            $staged = Get-MemberPath $operation ('stage/' + $item.Target)
            Copy-Verified $item.Source $staged $item.Sha256
        }
        $record.status = 'in_progress'
        Write-JsonAtomic $recordPath $record
        Assert-ProcessesStopped
        foreach ($item in $preview.Items) {
            Assert-ProcessesStopped
            $dest = Get-MemberPath $preview.Client $item.Target
            $src = Get-MemberPath $operation ('stage/' + $item.Target)
            if ((Get-Sha256 $src) -ne $item.Sha256) { throw "暂存文件身份变化：$($item.Target)" }
            if ($item.ReplacesOriginal) {
                if (-not [IO.File]::Exists($dest) -or (Get-Sha256 $dest) -ne $lock.original_d3d9_sha256) {
                    throw '原始 d3d9.dll 已变化。'
                }
            } elseif ([IO.File]::Exists($dest) -or [IO.Directory]::Exists($dest)) {
                throw "目标文件已出现：$($item.Target)"
            }
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dest)) | Out-Null
            Assert-NoReparse $dest
            $temp = "$dest.dlss5-installing"
            if ([IO.File]::Exists($temp) -or [IO.Directory]::Exists($temp)) { throw "暂存路径已存在：$temp" }
            try { Copy-Verified $src $temp $item.Sha256 }
            catch {
                if ([IO.File]::Exists($temp) -or [IO.Directory]::Exists($temp)) {
                    $record.errors = @($record.errors) + @("暂存文件需要检查：$temp")
                }
                throw
            }
            $pendingTemps.Add([pscustomobject]@{ Path = $temp; Sha256 = $item.Sha256 })
            if ($item.ReplacesOriginal) { Move-AtomicReplace $temp $dest }
            else { [IO.File]::Move($temp, $dest) }
            $changed.Add($item)
            if ((Get-Sha256 $dest) -ne $item.Sha256) { throw "安装回读不一致：$($item.Target)" }
            $record.applied = @($record.applied) + @($item.Target)
            Write-JsonAtomic $recordPath $record
        }
        $record.status = 'completed'
        Write-JsonAtomic $recordPath $record
        return [pscustomobject]@{ Status = 'completed'; Operation = $operation; Count = $changed.Count }
    } catch {
        $cause = $_.Exception.Message
        foreach ($tempInfo in $pendingTemps) {
            try {
                if ([IO.File]::Exists($tempInfo.Path)) {
                    if ((Get-Sha256 $tempInfo.Path) -ne $tempInfo.Sha256) { throw '暂存文件变化，保留现状。' }
                    [IO.File]::Delete($tempInfo.Path)
                }
            } catch { $record.errors = @($record.errors) + @("暂存文件：$($_.Exception.Message)") }
        }
        for ($index = $changed.Count - 1; $index -ge 0; $index--) {
            $item = $changed[$index]
            try {
                $dest = Get-MemberPath $preview.Client $item.Target
                if (-not [IO.File]::Exists($dest) -or (Get-Sha256 $dest) -ne $item.Sha256) {
                    throw '已安装文件变化，保留现状。'
                }
                if ($item.ReplacesOriginal) {
                    $temp = "$dest.dlss5-rollback"
                    Copy-Verified $backup $temp $lock.original_d3d9_sha256
                    Move-AtomicReplace $temp $dest
                    if ((Get-Sha256 $dest) -ne $lock.original_d3d9_sha256) { throw '原件恢复回读失败。' }
                } else { [IO.File]::Delete($dest) }
            } catch { $record.errors = @($record.errors) + @("$($item.Target)：$($_.Exception.Message)") }
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
    if ($record.schema -ne 1 -or $record.status -notin @('completed', 'in_progress', 'rollback_incomplete') -or
        @($record.members).Count -ne @($lock.members).Count) {
        throw '安装记录不受支持或已还原。'
    }
    $client = Assert-Client ([string]$record.client) $lock
    foreach ($expected in $lock.members) {
        $actual = @($record.members | Where-Object { $_.target -eq $expected.target })
        if ($actual.Count -ne 1 -or $actual[0].sha256 -ne $expected.sha256 -or
            [bool]$actual[0].replaces_original -ne [bool]$expected.replaces_original) {
            throw '安装记录与组件清单不一致。'
        }
    }
    $backup = Get-MemberPath $operationFull 'backup/d3d9.dll'
    if (-not [IO.File]::Exists($backup) -or (Get-Sha256 $backup) -ne $lock.original_d3d9_sha256) {
        throw '原始 d3d9.dll 备份缺失或身份不符。'
    }
    return [pscustomobject]@{ Directory = $operationFull; RecordPath = $recordPath;
        Record = $record; Client = $client; Backup = $backup; Lock = $lock }
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
    $conflictRoot = Get-MemberPath $verified.Directory ("modified-$runId")
    $report = [ordered]@{ status = 'in_progress'; saved_modified = @(); restored = @(); errors = @() }
    Write-JsonAtomic $reportPath $report
    $members = @($verified.Lock.members)
    [array]::Reverse($members)
    foreach ($member in $members) {
        try {
            Assert-ProcessesStopped
            $dest = Get-MemberPath $verified.Client ([string]$member.target)
            if ([IO.Directory]::Exists($dest)) { throw '目标位置是目录，保留现状。' }
            $exists = [IO.File]::Exists($dest)
            $current = if ($exists) { Get-Sha256 $dest } else { $null }
            if ($verified.Record.status -ne 'completed' -and $exists -and $current -ne $member.sha256 -and
                (-not $member.replaces_original -or $current -ne $verified.Lock.original_d3d9_sha256) -and
                $member.target -notin @($verified.Record.applied)) {
                throw '文件不在已应用记录中且身份不符，保留现状。'
            }
            if ($exists -and $current -ne $member.sha256) {
                $saved = Get-MemberPath $conflictRoot ([string]$member.target)
                Copy-Verified $dest $saved $current
                $report.saved_modified = @($report.saved_modified) + @([string]$member.target)
            }
            if ($exists -and (Get-Sha256 $dest) -ne $current) { throw '文件在恢复期间变化，保留现状。' }
            if ($member.replaces_original) {
                $temp = "$dest.dlss5-restoring-$runId"
                Copy-Verified $verified.Backup $temp $verified.Lock.original_d3d9_sha256
                if ($exists) { Move-AtomicReplace $temp $dest }
                else { [IO.File]::Move($temp, $dest) }
                if ((Get-Sha256 $dest) -ne $verified.Lock.original_d3d9_sha256) { throw '原件回读失败。' }
            } elseif ($exists) {
                [IO.File]::Delete($dest)
            }
            $pending = "$dest.dlss5-installing"
            if ([IO.File]::Exists($pending) -or [IO.Directory]::Exists($pending)) {
                throw '安装暂存路径仍存在，已保留并需要人工检查。'
            }
            $report.restored = @($report.restored) + @([string]$member.target)
        } catch { $report.errors = @($report.errors) + @("$($member.target)：$($_.Exception.Message)") }
    }
    if (@($report.errors).Count) {
        $report.status = 'rollback_incomplete'
    } else {
        $report.status = 'rolled_back'
        $verified.Record.status = 'rolled_back'
        $verified.Record | Add-Member -NotePropertyName restore_record -NotePropertyValue $reportPath -Force
        Write-JsonAtomic $verified.RecordPath $verified.Record
    }
    Write-JsonAtomic $reportPath $report
    return [pscustomobject]@{ Status = $report.status; Report = $reportPath;
        SavedModified = @($report.saved_modified).Count; Errors = @($report.errors) }
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
    foreach ($member in $verified.Lock.members) {
        $staged = Get-MemberPath $verified.Directory ('stage/' + $member.target)
        if (-not [IO.File]::Exists($staged) -or (Get-Sha256 $staged) -ne $member.sha256) {
            throw "修复来源缺失或身份不符：$($member.target)"
        }
    }
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
    if ([double]::IsNaN($Intensity) -or [double]::IsInfinity($Intensity) -or $Intensity -lt 0 -or $Intensity -gt 2) {
        throw 'NR 强度必须在 0 到 2 之间。'
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
        'host64/nvngx_dlss.dll' = 'x64'; 'host64/nvngx_dlssnr.dll' = 'x64'
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
    $gameIdentity = if ([IO.File]::Exists($exe) -and (Get-Sha256 $exe) -eq $lock.game_sha256) { '匹配' } else { '缺失或不匹配' }
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
    return [pscustomobject]@{ GameIdentity = $gameIdentity; Files = $files; Config = [pscustomobject]$config;
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
    foreach ($pair in @(@('强度',$Intensity),@('色调结构',$LocalTone),@('结构强度',$LocalStructure))) {
        $v = [double]$pair[1]
        if ([double]::IsNaN($v) -or [double]::IsInfinity($v) -or $v -lt 0 -or $v -gt 2) {
            throw ($pair[0] + ' 必须在 0 到 2 之间。')
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

Export-ModuleMember -Function Get-Dlss5InstallPreview, Invoke-Dlss5Install, Get-Dlss5Operations,
    Invoke-Dlss5Restore, Invoke-Dlss5Repair, Get-Dlss5Status, Set-Dlss5Settings, Set-Dlss5Pipeline,
    Export-Dlss5Diagnostics, Start-Dlss5WindowWatch, Get-Dlss5AdvancedSettings, Set-Dlss5AdvancedSettings,
    Get-Dlss5Schemes, Save-Dlss5Scheme, Apply-Dlss5Scheme
