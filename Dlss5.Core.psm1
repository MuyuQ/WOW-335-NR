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
# zh: 显卡信息短期缓存；文件身份和路径检查不能使用长期缓存。
$script:GpuCache = $null
$script:GpuCacheAt = [DateTime]::MinValue
$script:StatusCache = $null
$script:StatusCacheAt = [DateTime]::MinValue
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

if (-not ('Wow335.Dlss5.ClientLock' -as [type])) {
    # R5：客户端维护锁——与原生控制中心（native/control_center/client_lock.hpp）共用同一个
    # 命名互斥量 Local\WOW335NR-Client-<卷序列号>-<文件索引高>-<文件索引低>。
    # 三个值取自客户端根目录自身的 GetFileInformationByHandle，与原生一致。
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Wow335.Dlss5 {
    public static class ClientLock {
        // R5 修复：BY_HANDLE_FILE_INFORMATION 里的三个 FILETIME 必须是两个 DWORD（低/高半部）。
        // 用 ulong 时默认 8 字节对齐会把 VolumeSerialNumber 顶到偏移 32（Win32 是 28），
        // 卷序列号与文件索引全部错位，算出的互斥量名与原生 client_lock.hpp 不一致，跨工具互斥失效。
        [StructLayout(LayoutKind.Sequential)]
        public struct FILETIME {
            public uint LowDateTime;
            public uint HighDateTime;
        }
        [StructLayout(LayoutKind.Sequential)]
        public struct BY_HANDLE_FILE_INFORMATION {
            public uint FileAttributes;
            public FILETIME CreationTime;
            public FILETIME LastAccessTime;
            public FILETIME LastWriteTime;
            public uint VolumeSerialNumber;
            public uint FileSizeHigh;
            public uint FileSizeLow;
            public uint NumberOfLinks;
            public uint FileIndexHigh;
            public uint FileIndexLow;
        }
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security,
            uint disposition, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool GetFileInformationByHandle(IntPtr handle, out BY_HANDLE_FILE_INFORMATION info);
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern uint GetFileType(IntPtr handle);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr CreateMutexW(IntPtr security, bool initialOwner, string name);
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool ReleaseMutex(IntPtr handle);
        // 与原生 GuardedPath 一致：目标根与每一级祖先都必须是本地磁盘上的真实目录，
        // 不能是文件、链接/重解析点（原生在 client_lock.hpp Acquire 里做同样的校验）。
        private static bool ReadDirectoryIdentity(string directory, out BY_HANDLE_FILE_INFORMATION info) {
            info = new BY_HANDLE_FILE_INFORMATION();
            // FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT：打开目录本身而不是链接目标。
            IntPtr handle = CreateFileW(directory, 0x80u, 0x3u, IntPtr.Zero, 3u, 0x02200000u, IntPtr.Zero);
            if (handle == IntPtr.Zero || handle == new IntPtr(-1)) { return false; }
            try {
                BY_HANDLE_FILE_INFORMATION value;
                if (!GetFileInformationByHandle(handle, out value)) { return false; }
                if ((value.FileAttributes & 0x10u) == 0u) { return false; }   // FILE_ATTRIBUTE_DIRECTORY
                if ((value.FileAttributes & 0x400u) != 0u) { return false; }  // FILE_ATTRIBUTE_REPARSE_POINT
                if (GetFileType(handle) != 1u) { return false; }              // FILE_TYPE_DISK
                info = value;
                return true;
            } finally { CloseHandle(handle); }
        }
        // 返回 [句柄十进制, 互斥量名, 状态]；状态 0=已持有 1=接管被遗弃的锁 2=被占用
        // -1=目录身份不可读 -2=创建互斥量失败。
        public static string[] Acquire(string directory) {
            BY_HANDLE_FILE_INFORMATION info = new BY_HANDLE_FILE_INFORMATION();
            bool gotInfo = false;
            string current = directory;
            while (!string.IsNullOrEmpty(current)) {
                BY_HANDLE_FILE_INFORMATION step;
                if (!ReadDirectoryIdentity(current, out step)) { return new string[] { "0", "", "-1" }; }
                if (!gotInfo) { info = step; gotInfo = true; }
                string parent = System.IO.Path.GetDirectoryName(current);
                if (string.IsNullOrEmpty(parent) || string.Equals(parent, current, StringComparison.OrdinalIgnoreCase)) { break; }
                current = parent;
            }
            if (!gotInfo) { return new string[] { "0", "", "-1" }; }
            string name = string.Format("Local\\WOW335NR-Client-{0:X8}-{1:X8}-{2:X8}",
                info.VolumeSerialNumber, info.FileIndexHigh, info.FileIndexLow);
            IntPtr mutex = CreateMutexW(IntPtr.Zero, false, name);
            if (mutex == IntPtr.Zero) { return new string[] { "0", name, "-2" }; }
            uint wait = WaitForSingleObject(mutex, 0u);
            if (wait == 0u) { return new string[] { mutex.ToInt64().ToString(), name, "0" }; }
            if (wait == 0x80u) { return new string[] { mutex.ToInt64().ToString(), name, "1" }; }
            CloseHandle(mutex);
            return new string[] { "0", name, "2" };
        }
        public static void Release(IntPtr handle) {
            if (handle == IntPtr.Zero) { return; }
            try { ReleaseMutex(handle); }
            finally { CloseHandle(handle); }
        }
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
        $attributes = $null
        try { $attributes = [IO.File]::GetAttributes($current) }
        catch [IO.FileNotFoundException] { }
        catch [IO.DirectoryNotFoundException] { }
        if ($null -ne $attributes -and ($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "路径含有链接或重解析点：$current"
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
    # zh: 用 .NET 流式哈希，不依赖 Get-FileHash cmdlet。子进程的 PSModulePath 被别的
    # PowerShell 7 会话污染时，Windows PowerShell 5.1 无法自动加载 Utility 模块，
    # Get-FileHash 会报「无法识别为 cmdlet」（20261113 实测控制台日志）。
    # 同大小文件可保留原时间戳，事务检查必须重新读取文件并拒绝并发写入。
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try { $hashBytes = $algorithm.ComputeHash($stream) } finally { $algorithm.Dispose() }
    } finally { $stream.Dispose() }
    $builder = [Text.StringBuilder]::new(64)
    foreach ($byte in $hashBytes) { [void]$builder.Append($byte.ToString('x2', [Globalization.CultureInfo]::InvariantCulture)) }
    return $builder.ToString()
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
    # R11 补充：内部代码会把条目建成 [ordered] 字典（事务 entries/repair_entries）。字典的
    # PSObject.Properties 只有 Count/Keys 等接口属性，不暴露键，必须走 IDictionary 分支，
    # 否则清理函数会把「已登记备份」误判为未登记而全部保留。
    if ($null -eq $Entry) { return $null }
    if ($Entry -is [Collections.IDictionary]) {
        if ($Entry.Contains($Name)) { return $Entry[$Name] }
        return $null
    }
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
    # A4：结构仍然严格校验；只有「target 不在当前组件锁中」不再让整条记录变成不可读，
    # 而是收集到 UnknownTargets，由安装/卸载/状态入口告警并保留这些条目（换锁后仍能处理已知部分）。
    $path = Get-Dlss5ClientRecordPath $Client
    if (-not [IO.File]::Exists($path)) { return $null }
    try {
        $record = [IO.File]::ReadAllText($path, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json
        if ($record.schema -ne 1 -or $record.status -ne 'installed' -or
            $record.entries -isnot [array] -or $record.entries.Count -eq 0) { return $null }
        $lock = Get-Dlss5Lock
        $seen = @{}
        $unknown = [Collections.Generic.List[string]]::new()
        foreach ($entry in $record.entries) {
            $target = [string]$entry.target
            $null = Get-MemberPath $Client $target
            if ($seen.ContainsKey($target) -or
                $entry.sha256 -isnot [string] -or $entry.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
                $entry.replaces_original -isnot [bool]) { return $null }
            $seen[$target] = $true
            if ($entry.pre_state -eq 'present') {
                if ($entry.backup -isnot [string] -or $entry.backup -ne $target -or
                    $entry.previous_sha256 -isnot [string] -or
                    $entry.previous_sha256 -cnotmatch '^[0-9a-f]{64}$') { return $null }
                $null = Get-MemberPath (Get-MemberPath $Client 'host64/dlss5-backup') $entry.backup
            } elseif ($entry.pre_state -ne 'absent' -or $null -ne $entry.backup -or
                      $null -ne $entry.previous_sha256) { return $null }
            $known = @($lock.members | Where-Object { [string]$_.target -eq $target })
            if ($known.Count -ne 1 -or [bool]$known[0].replaces_original -ne [bool]$entry.replaces_original) {
                $unknown.Add($target)
            }
        }
        return [pscustomobject]@{ Path = $path; Record = $record;
            BackupRoot = (Get-MemberPath $Client 'host64/dlss5-backup');
            UnknownTargets = @($unknown) }
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
    $backupRoot = Join-Path $clientFull 'host64/dlss5-backup'
    # A1：预览阶段就核对「上次失败安装遗留的原件备份」是否可复用；来源不明的备份在这里报冲突，
    # 而不是等安装进行到备份步骤才失败。
    $priorRecord = $null
    try { $priorRecord = Test-Dlss5ClientRecord $clientFull } catch { $priorRecord = $null }
    $priorEntries = if ($null -ne $priorRecord) { @($priorRecord.Record.entries) } else { @() }
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
        if ($preState -eq 'present' -and $null -eq $issue) {
            $backupPath = $null
            try { $backupPath = Get-MemberPath $backupRoot $targetName }
            catch { $issue = '备份目录冲突：备份路径无效或包含链接；请先手动检查 host64\dlss5-backup' }
            if ($null -ne $backupPath) {
                if ([IO.Directory]::Exists($backupPath)) {
                    $issue = "备份目录冲突：host64\dlss5-backup\$targetName 已存在且是目录；请先手动移走后重试"
                } elseif ([IO.File]::Exists($backupPath)) {
                    $backupSha = Get-Sha256 $backupPath
                    $reusable = ($backupSha -eq $existingSha)
                    if (-not $reusable) {
                        foreach ($priorEntry in $priorEntries) {
                            if ([string](Get-ManifestField $priorEntry 'target') -ne $targetName) { continue }
                            if ([string](Get-ManifestField $priorEntry 'sha256') -eq $backupSha -or
                                [string](Get-ManifestField $priorEntry 'previous_sha256') -eq $backupSha) { $reusable = $true; break }
                        }
                    }
                    if (-not $reusable) {
                        $issue = "备份目录冲突：host64\dlss5-backup\$targetName 已存在且无法核对来源；请先手动移走该备份后重试"
                    }
                }
            }
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
    $clientFull = $preview.Client
    Enter-Dlss5ClientLock $clientFull
    try {
        Assert-ProcessesStopped
        $null = Remove-Dlss5StaleTemp $clientFull
        $priorRecord = Test-Dlss5ClientRecord $clientFull
        $clientRecordPath = Get-Dlss5ClientRecordPath $clientFull
        if ([IO.File]::Exists($clientRecordPath) -and $null -eq $priorRecord) {
            throw '已有安装记录不可读，保留现状；请先恢复记录。'
        }
        $warnings = @()
        if ($null -ne $priorRecord -and @($priorRecord.UnknownTargets).Count) {
            $warnings += ('客户端安装记录含当前组件锁无法识别的条目（{0} 项），将原样保留：{1}' -f
                @($priorRecord.UnknownTargets).Count, (@($priorRecord.UnknownTargets) -join '、'))
        }
        $items = @($preview.Items | Where-Object { $_.PreState -notin @('same', 'missing-optional') })
        if ($items.Count -eq 0) {
            return [pscustomobject]@{ Status = 'already_current'; Operation = $null; Count = 0;
                Client = $clientFull; Record = $clientRecordPath; Warnings = @($warnings) }
        }
        $manifest = Get-Dlss5Manifest
        Assert-NoReparse $script:OperationsRoot
        [IO.Directory]::CreateDirectory($script:OperationsRoot) | Out-Null
        $operation = Join-Path $script:OperationsRoot ([guid]::NewGuid().ToString('N'))
        if (Test-Path -LiteralPath $operation) { throw '事务目录已存在。' }
        # zh: 原件备份也放客户端内（host64\dlss5-backup）——
        # 卸载时即使草稿区已清理、换机，也能按客户端内记录还原（20261112）。
        $backupRoot = Get-MemberPath $clientFull 'host64/dlss5-backup'
        $entries = [Collections.Generic.List[object]]::new()
        foreach ($item in $items) {
            $backupRel = if ($item.PreState -eq 'present') { [string]$item.Target } else { $null }
            $entries.Add([ordered]@{ target = [string]$item.Target; sha256 = [string]$item.Sha256;
                replaces_original = [bool]$item.ReplacesOriginal; pre_state = [string]$item.PreState;
                previous_sha256 = $item.ExistingSha256; backup = $backupRel })
        }
        # Keep the first-install snapshot for each member; a later install owns only its delta.
        $allEntries = @()
        if ($null -ne $priorRecord) { $allEntries += @($priorRecord.Record.entries) }
        foreach ($entry in $entries) {
            if (@($allEntries | Where-Object { $_.target -eq $entry.target }).Count -eq 0) {
                $allEntries += $entry
            }
        }
        $record = $null
        $recordPath = $null
        $changed = [Collections.Generic.List[object]]::new()
        $pendingTemps = [Collections.Generic.List[object]]::new()
        $createdBackups = [Collections.Generic.List[object]]::new()
        try {
            [IO.Directory]::CreateDirectory($operation) | Out-Null
            Assert-NoReparse $operation
            $recordPath = Join-Path $operation 'transaction.json'
            # A1：先把 schema=2 / preparing 事务（含 entries、repair_entries、previous_client_record）
            # 完整落盘，再开始任何备份/安装写入；失败时据此回滚，不留「无记录、无法重试」的状态。
            $record = [ordered]@{ schema = 2; status = 'preparing'; client = $clientFull; stage = $preview.Stage;
                manifest_version = $preview.ManifestVersion; tool = 'WOW-335-NR';
                created_utc = [DateTime]::UtcNow.ToString('o');
                entries = @($entries); repair_entries = @($allEntries);
                previous_client_record = $(if ($null -ne $priorRecord) { $priorRecord.Record } else { $null });
                applied = @(); errors = @() }
            Write-JsonAtomic $recordPath $record
            [IO.Directory]::CreateDirectory($backupRoot) | Out-Null
            Assert-NoReparse $backupRoot
            $reusedBackups = @()
            foreach ($item in $items) {
                if ($item.PreState -ne 'present') { continue }
                $backupRel = [string]$item.Target
                $backupPath = Get-MemberPath $backupRoot $backupRel
                $expected = [string]$item.ExistingSha256
                if ([IO.File]::Exists($backupPath)) {
                    # A1：同名备份已存在——只有当它就是上次失败安装留下的同内容原件时才复用，
                    # 否则视为冲突直接失败（预览阶段也应已报出）。
                    if ((Get-Sha256 $backupPath) -ne $expected) {
                        throw "备份目录冲突：$backupPath 已存在且与当前原件不符；请先手动移走该备份后重试。"
                    }
                    $reusedBackups += $backupRel
                    Write-Dlss5CoreDebug ("reusing leftover backup: {0}" -f $backupPath)
                } elseif ([IO.Directory]::Exists($backupPath)) {
                    throw "备份目录冲突：$backupPath 是目录；请先手动移走后重试。"
                } else {
                    Copy-Verified $item.TargetPath $backupPath $expected
                    $createdBackups.Add([pscustomobject]@{ Path = $backupPath; Sha256 = $expected })
                }
                Copy-Verified $item.TargetPath (Get-MemberPath $operation ('backup/' + $backupRel)) $expected
            }
            if (@($reusedBackups).Count) {
                Write-Dlss5CoreDebug ("reused {0} leftovers from a previous failed install" -f @($reusedBackups).Count)
            }
            foreach ($item in @($preview.Items | Where-Object { $_.PreState -ne 'missing-optional' })) {
                $staged = Get-MemberPath $operation ('stage/' + $item.Target)
                Copy-Verified $item.Source $staged ([string]$item.Sha256)
            }
            Write-Dlss5CoreDebug ("staged {0} members; entering install phase" -f $items.Count)
            $record.status = 'in_progress'
            Write-JsonAtomic $recordPath $record
            Assert-ProcessesStopped
            foreach ($item in $items) {
                Assert-ProcessesStopped
                $dest = Get-MemberPath $clientFull ([string]$item.Target)
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
                installed_utc = [DateTime]::UtcNow.ToString('o'); client = $clientFull;
                manifest_version = $preview.ManifestVersion; tool = 'WOW-335-NR'; operation = $operation;
                entries = @($allEntries); residue = @($manifest.Residue) }
            Write-JsonAtomic $clientRecordPath $clientRecord
            Write-Dlss5CoreDebug ("install completed: {0} members, operation {1}" -f $changed.Count, $operation)
            return [pscustomobject]@{ Status = 'completed'; Operation = $operation; Count = $changed.Count;
                Client = $clientFull; Record = $clientRecordPath; Warnings = @($warnings) }
        } catch {
            $cause = $_.Exception.Message
            Write-Dlss5CoreDebug ("install failed, rolling back: {0}" -f $cause)
            if ($null -eq $record -or $null -eq $recordPath) { throw "安装未完成（尚未建立事务记录）：$cause" }
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
            $rollbackReport = Join-Path $operation 'rollback.json'
            if ($rollbackEntries.Count) {
                $null = Invoke-Dlss5Teardown -ClientFull $clientFull -Entries $rollbackEntries `
                    -BackupRoot $backupRoot -SaveRoot (Join-Path $operation 'rollback-modified') -ReportPath $rollbackReport
                try {
                    $saved = [IO.File]::ReadAllText($rollbackReport, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json
                    $record.errors = @($record.errors) + @($saved.errors)
                } catch { }
            } else {
                Write-JsonAtomic $rollbackReport ([ordered]@{ status = 'rolled_back'; saved_modified = @();
                    restored = @(); deleted = @(); errors = @(); report = $rollbackReport })
            }
            $record.status = if (@($record.errors).Count) { 'rollback_incomplete' } else { 'rolled_back' }
            $record.errors = @($record.errors) + @("安装错误：$cause")
            # A1：只有确认回滚完成才删除本次新建的备份/临时件与整个事务目录；
            # 回滚不完整时保留事务与备份，供「恢复安装前文件」继续处理，避免永久锁死。
            $cleanupErrors = @()
            if ($record.status -eq 'rolled_back') {
                foreach ($info in $createdBackups) {
                    try {
                        if ([IO.File]::Exists($info.Path) -and (Get-Sha256 $info.Path) -eq $info.Sha256) { [IO.File]::Delete($info.Path) }
                    } catch { $cleanupErrors += @("备份清理：$($_.Exception.Message)") }
                }
                try { $null = Remove-EmptyDirectoryTree $backupRoot }
                catch { $cleanupErrors += @("清理空备份目录：$($_.Exception.Message)") }
                if (@($cleanupErrors).Count) { $record.status = 'rollback_incomplete' }
            }
            Write-JsonAtomic $recordPath $record
            if ($record.status -eq 'rolled_back') {
                try { [IO.Directory]::Delete($operation, $true) }
                catch { Write-Dlss5CoreDebug ("operation cleanup skipped: {0}" -f $_.Exception.Message) }
                throw "安装未完成（已回滚，本次事务已清理）：$cause"
            }
            throw "安装未完成（rollback_incomplete），事务记录：$recordPath；$cause。请在控制中心用「恢复安装前文件」完成回滚后重试。"
        }
    } finally { Exit-Dlss5ClientLock $clientFull }
}

function Get-VerifiedOperation([string]$Operation) {
    $lock = Get-Dlss5Lock
    $operationFull = Get-CanonicalPath $Operation
    if (-not (Test-Within $operationFull $script:OperationsRoot) -or
        -not ([IO.Path]::GetDirectoryName($operationFull).Equals($script:OperationsRoot, [StringComparison]::OrdinalIgnoreCase))) {
        throw '事务记录不在本 PowerShell 入口的受控目录；请使用原安装入口修复或恢复，不要手动移动记录。'
    }
    Assert-NoReparse $operationFull
    $recordPath = Get-MemberPath $operationFull 'transaction.json'
    $record = [IO.File]::ReadAllText($recordPath, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json
    if ($record.schema -notin @(1, 2) -or $record.status -notin @('preparing', 'completed', 'in_progress', 'rollback_incomplete')) {
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
        if ($entry.pre_state -ne 'present' -or [string]$record.status -eq 'preparing') { continue }
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

# ---- R5：客户端维护锁（与原生共享命名互斥量） ----
$script:ClientLockEntries = @{}
function Enter-Dlss5ClientLock {
    # R5：任何写事务都必须在第一次写入前确认真正拿到锁。身份不可读、建锁失败、handle=0、
    # 或锁被别的维护入口占用时一律抛错（不再只写 debug 后继续写）。
    # abandoned（上一个维护进程异常结束）只允许恢复/卸载/清理类入口用 -Recovery 接管；
    # 安装/修复必须拒绝，对齐原生 client_lock.hpp「abandoned 需要恢复、不能新建变更」。
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Client, [switch]$Recovery)
    $key = (Get-CanonicalPath $Client).ToLowerInvariant()
    if ($script:ClientLockEntries.ContainsKey($key)) { $script:ClientLockEntries[$key].Depth++; return }
    if (-not ('Wow335.Dlss5.ClientLock' -as [type])) {
        throw '客户端维护锁组件不可用，已在写入前停止；没有跨工具互斥保护时不允许修改客户端。'
    }
    $result = [Wow335.Dlss5.ClientLock]::Acquire($key)
    $status = [int]$result[2]
    $handleValue = [int64]$result[0]
    if ($status -eq 2) {
        throw '另一个维护入口正在操作该客户端（原生控制中心或另一个维护窗口）。请关闭正在进行的维护操作后重试。'
    }
    if ($status -eq 1) {
        if (-not $Recovery) {
            if ($handleValue -ne 0) { [Wow335.Dlss5.ClientLock]::Release([IntPtr]::new($handleValue)) }
            throw '客户端维护锁被上一次异常结束的维护进程遗弃（abandoned）。安装/修复不会在未恢复的客户端上继续；请先执行「恢复安装前文件」「卸载」或「清理待完成操作」完成恢复后再试。'
        }
        if ($handleValue -eq 0) {
            throw '客户端维护锁已被遗弃，但无法接管（句柄为空）；已在写入前停止。请关闭相关维护窗口后重试。'
        }
        Write-Dlss5CoreDebug 'client mutex was abandoned by a previous owner; recovery entry takes it over'
    } elseif ($status -in @(-1, -2) -or $handleValue -eq 0) {
        throw ("无法建立客户端维护锁（状态 {0}）；已在写入前停止，请检查客户端目录是否可访问后重试。" -f $status)
    }
    $script:ClientLockEntries[$key] = [pscustomobject]@{
        Handle = [IntPtr]::new($handleValue); Name = [string]$result[1]; Depth = 1; Abandoned = ($status -eq 1) }
    Write-Dlss5CoreDebug ("client lock acquired: {0}" -f [string]$result[1])
}

function Exit-Dlss5ClientLock([string]$Client) {
    try {
        $key = (Get-CanonicalPath $Client).ToLowerInvariant()
        if (-not $script:ClientLockEntries.ContainsKey($key)) { return }
        $entry = $script:ClientLockEntries[$key]
        $entry.Depth--
        if ($entry.Depth -le 0) {
            [Wow335.Dlss5.ClientLock]::Release([IntPtr]$entry.Handle)
            $script:ClientLockEntries.Remove($key)
        }
    } catch { Write-Dlss5CoreDebug ("client lock release failed: {0}" -f $_.Exception.Message) }
}

function Remove-EmptyDirectoryTree([string]$Root) {
    # 只删空目录；遇到链接/重解析点直接跳过，不跟随（与原生 RemoveEmptyBackupDirectories 对齐）。
    if (-not [IO.Directory]::Exists($Root)) { return 0 }
    $order = [Collections.Generic.List[string]]::new()
    $stack = [Collections.Generic.Stack[string]]::new()
    $stack.Push($Root)
    while ($stack.Count -gt 0) {
        $current = $stack.Pop()
        $order.Add($current)
        try {
            foreach ($child in [IO.Directory]::EnumerateDirectories($current)) {
                try {
                    $attrs = [IO.File]::GetAttributes($child)
                    if (($attrs -band [IO.FileAttributes]::ReparsePoint) -ne 0) { continue }
                } catch { continue }
                $stack.Push($child)
            }
        } catch { }
    }
    $removed = 0
    foreach ($dir in ($order | Sort-Object -Property Length -Descending)) {
        try {
            if ([IO.Directory]::EnumerateFileSystemEntries($dir).GetEnumerator().MoveNext()) { continue }
            [IO.Directory]::Delete($dir, $false)
            $removed++
        } catch { }
    }
    return $removed
}

function Remove-Dlss5RecordBackups {
    # R11：按「记录登记 + previous_sha256 校验」逐文件清理客户端内原件备份。
    # 未登记的文件、内容与 previous_sha256 不符的备份一律保留；最后只删空目录，绝不整树删除。
    # 恢复（正常回滚）、卸载、Invoke-Dlss5CleanupPending 三条路径共用本函数。
    param([Parameter(Mandatory)][string]$ClientFull, [Parameter(Mandatory)][object[]]$Entries)
    $backupRoot = Get-MemberPath $ClientFull 'host64/dlss5-backup'
    $previousByBackup = [ordered]@{}
    foreach ($entry in @($Entries)) {
        if ($null -eq $entry) { continue }
        $backupRel = [string](Get-ManifestField $entry 'backup')
        if (-not $backupRel) { continue }
        $previous = [string](Get-ManifestField $entry 'previous_sha256')
        if (-not $previous) { continue }
        if ($previousByBackup.Contains($backupRel)) { continue }
        $previousByBackup[$backupRel] = $previous
    }
    $removed = [Collections.Generic.List[string]]::new()
    $kept = [Collections.Generic.List[string]]::new()
    $errors = [Collections.Generic.List[string]]::new()
    foreach ($backupRel in @($previousByBackup.Keys)) {
        try {
            $backupPath = Get-MemberPath $backupRoot ([string]$backupRel)
            if (-not [IO.File]::Exists($backupPath)) { continue }
            if (-not (Test-Within $backupPath $backupRoot)) { throw '备份路径越界，保留。' }
            $previous = [string]$previousByBackup[$backupRel]
            if ((Get-Sha256 $backupPath) -ne $previous) {
                $kept.Add([string]$backupRel)
                Write-Dlss5CoreDebug ("cleanup keeps unverified backup: {0}" -f $backupPath)
                continue
            }
            [IO.File]::Delete($backupPath)
            $removed.Add([string]$backupRel)
        } catch { $errors.Add("备份 $backupRel：$($_.Exception.Message)") }
    }
    # 未登记（以及上面哈希不符）的文件必须保留并报告，绝不整树删除。
    # R11：逐层枚举，遇到子目录重解析点（junction/symlink）只记录链接本身，不跟随枚举，
    # 也不把链接目标里的文件误报成本工具的未登记备份。
    $links = [Collections.Generic.List[string]]::new()
    try {
        if ([IO.Directory]::Exists($backupRoot)) {
            Assert-NoReparse $backupRoot
            $pending = [Collections.Generic.Stack[string]]::new()
            $pending.Push($backupRoot)
            while ($pending.Count -gt 0) {
                $directory = $pending.Pop()
                foreach ($file in [IO.Directory]::EnumerateFiles($directory, '*', [IO.SearchOption]::TopDirectoryOnly)) {
                    $leftoverRel = $file.Substring($backupRoot.Length).TrimStart('\', '/')
                    if (-not $kept.Contains($leftoverRel)) { $kept.Add($leftoverRel) }
                }
                foreach ($child in [IO.Directory]::EnumerateDirectories($directory, '*', [IO.SearchOption]::TopDirectoryOnly)) {
                    $childInfo = [IO.DirectoryInfo]::new($child)
                    if (($childInfo.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                        $linkRel = $child.Substring($backupRoot.Length).TrimStart('\', '/')
                        if (-not $links.Contains($linkRel)) { $links.Add($linkRel) }
                        if (-not $kept.Contains($linkRel)) { $kept.Add($linkRel) }
                        continue
                    }
                    $pending.Push($child)
                }
            }
        }
    } catch { $errors.Add("枚举保留备份：$($_.Exception.Message)") }
    try { $null = Remove-EmptyDirectoryTree $backupRoot }
    catch { $errors.Add("清理空备份目录：$($_.Exception.Message)") }
    return [pscustomobject]@{ BackupRoot = $backupRoot; Removed = @($removed); Kept = @($kept);
        Links = @($links); Errors = @($errors) }
}

function Undo-Dlss5RepairWrites {
    # R1：修复执行中失败时，按写入日志把客户端文件回滚到修复前状态。
    # replace 用 repair-modified-<runId> 里的原件副本（先复制到目标同目录临时文件再原子替换）；
    # create 只在目标仍是本次写入的内容时删除，避免误删用户或其他程序后来写入的文件。
    # R1（零写入）：允许空 journal。首个需要修复的成员在写入目标前就失败时，没有任何
    # 已完成的写入可回滚；此时必须正常返回 RolledBack=0，由调用方保留原始复制/Move 错误，
    # 而不是让 Mandatory 参数绑定错误掩盖真正的失败原因。
    param([Parameter(Mandatory)][string]$ClientFull,
        [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Journal,
        [Parameter(Mandatory)][string]$RunId)
    $rolledBack = 0
    $errors = @()
    $items = @($Journal)
    for ($index = $items.Count - 1; $index -ge 0; $index--) {
        $entry = $items[$index]
        if ($null -eq $entry) { continue }
        $target = [string](Get-ManifestField $entry 'target')
        $dest = Get-MemberPath $ClientFull $target
        try {
            $temp = "$dest.dlss5-repair-$RunId"
            if ([IO.File]::Exists($temp)) { [IO.File]::Delete($temp) }
            if ([string](Get-ManifestField $entry 'action') -eq 'replace') {
                $backup = [string](Get-ManifestField $entry 'backup')
                $previous = [string](Get-ManifestField $entry 'previous_sha256')
                if ([IO.File]::Exists($dest) -and (Get-Sha256 $dest) -ne [string](Get-ManifestField $entry 'sha256')) {
                    throw '目标在修复后被其他程序修改，保留现状。'
                }
                if ([IO.File]::Exists($backup)) {
                    Copy-Verified $backup $temp $previous
                    Move-AtomicReplace $temp $dest
                } elseif (-not [IO.File]::Exists($dest) -or (Get-Sha256 $dest) -ne $previous) {
                    throw '原件副本缺失，无法回滚。'
                }
                if (-not [IO.File]::Exists($dest) -or (Get-Sha256 $dest) -ne $previous) { throw '回滚回读哈希与原件不符。' }
                $rolledBack++
            } else {
                if ([IO.File]::Exists($dest)) {
                    if ((Get-Sha256 $dest) -eq [string](Get-ManifestField $entry 'sha256')) { [IO.File]::Delete($dest); $rolledBack++ }
                    else { throw '目标在修复后被其他程序修改，保留现状。' }
                }
            }
        } catch { $errors += @("回滚 $target：$($_.Exception.Message)") }
    }
    return [pscustomobject]@{ RolledBack = $rolledBack; Errors = @($errors) }
}

function Remove-Dlss5StaleTemp([string]$Client) {
    # A5：只清理「能确认归属」的陈旧临时件：
    #  * 客户端侧：去掉工具后缀后的路径必须等于本客户端某个事务（当前或历史）条目的 target；
    #  * 事务目录侧：只清本客户端事务目录里 Write-JsonAtomic 留下的 <报告>.new-<guid>；
    #  * 存在活动事务、事务枚举失败、或无法确认归属时一律保留并报告。
    # 同目录同后缀的普通文件、其他客户端的临时件、没有事务依据的残留都不会被误删。
    $removed = [Collections.Generic.List[string]]::new()
    $kept = [Collections.Generic.List[string]]::new()
    $errors = [Collections.Generic.List[string]]::new()
    try {
        $clientFull = Get-CanonicalPath $Client
        $cutoff = [DateTime]::UtcNow.AddMinutes(-10)
        $suffixPattern = '(\.dlss5-installing|\.dlss5-restoring|\.dlss5-repair-[0-9a-fA-F]{32}|\.dlss5-settings-[0-9a-fA-F-]{8,})$'
        # 1) 枚举本客户端的事务记录（含历史）；无法枚举时整体保留，绝不盲删。
        $operations = [Collections.Generic.List[object]]::new()
        try {
            if ([IO.Directory]::Exists($script:OperationsRoot)) {
                Assert-NoReparse $script:OperationsRoot
                foreach ($directory in [IO.Directory]::EnumerateDirectories($script:OperationsRoot, '*', [IO.SearchOption]::TopDirectoryOnly)) {
                    $attrs = [IO.File]::GetAttributes($directory)
                    if (($attrs -band [IO.FileAttributes]::ReparsePoint) -ne 0) { continue }
                    $transactionPath = Join-Path $directory 'transaction.json'
                    if (-not [IO.File]::Exists($transactionPath)) { continue }
                    $transaction = [IO.File]::ReadAllText($transactionPath, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json
                    if ([string](Get-ManifestField $transaction 'client') -ne $clientFull) { continue }
                    $targets = [Collections.Generic.List[string]]::new()
                    foreach ($name in @('members', 'entries', 'repair_entries')) {
                        $property = $transaction.PSObject.Properties[$name]
                        if ($null -eq $property -or $null -eq $property.Value) { continue }
                        foreach ($item in @($property.Value)) {
                            if ($null -eq $item) { continue }
                            $target = [string](Get-ManifestField $item 'target')
                            if ($target) { $targets.Add($target) }
                        }
                    }
                    $operations.Add([pscustomobject]@{ Directory = $directory;
                        Status = [string](Get-ManifestField $transaction 'status');
                        Targets = @($targets | Select-Object -Unique) })
                }
            }
        } catch {
            $errors.Add("无法枚举事务，保留全部临时件：$($_.Exception.Message)")
            Write-Dlss5CoreDebug ("stale temp cleanup skipped: cannot enumerate transactions: {0}" -f $_.Exception.Message)
            return [pscustomobject]@{ Removed = @(); Kept = @(); Errors = @($errors) }
        }
        $active = @($operations | Where-Object { $_.Status -in @('preparing', 'in_progress') })
        if ($active.Count -gt 0) {
            $kept.Add("存在活动事务（$($active[0].Directory)），保留全部临时件")
            Write-Dlss5CoreDebug 'stale temp cleanup skipped: an active transaction exists'
            return [pscustomobject]@{ Removed = @(); Kept = @($kept); Errors = @() }
        }
        # 2) 归属集合：事务条目的 target（安装/恢复/修复/设置的临时件都写在 target 旁边）。
        $ownedBases = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($operation in $operations) {
            foreach ($target in $operation.Targets) {
                try { [void]$ownedBases.Add((Get-MemberPath $clientFull $target)) } catch { }
            }
        }
        # 3) 客户端目录：只删 base 路径确实登记在事务条目里的临时件。
        $dirs = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        [void]$dirs.Add($clientFull)
        try {
            $lock = Get-Dlss5Lock
            foreach ($member in $lock.members) {
                try { [void]$dirs.Add([IO.Path]::GetDirectoryName((Get-MemberPath $clientFull ([string]$member.target)))) } catch { }
            }
        } catch { }
        [void]$dirs.Add([IO.Path]::GetFullPath((Join-Path $clientFull 'host64/dlss5-backup')))
        foreach ($dir in $dirs) {
            if (-not [IO.Directory]::Exists($dir)) { continue }
            try { Assert-NoReparse $dir } catch { $kept.Add($dir); continue }
            try { $files = @([IO.Directory]::EnumerateFiles($dir, '*', [IO.SearchOption]::TopDirectoryOnly)) } catch { $kept.Add($dir); continue }
            foreach ($file in $files) {
                try {
                    if ([IO.Path]::GetFileName($file) -notmatch $suffixPattern) { continue }
                    if ([IO.File]::GetLastWriteTimeUtc($file) -gt $cutoff) { continue }
                    Assert-NoReparse $file
                    $basePath = $file -replace $suffixPattern, ''
                    if (-not $ownedBases.Contains($basePath)) {
                        $kept.Add($file)
                        Write-Dlss5CoreDebug ("stale temp kept (no transaction owns it): {0}" -f $file)
                        continue
                    }
                    [IO.File]::Delete($file)
                    $removed.Add($file)
                    Write-Dlss5CoreDebug ("stale temp removed: {0}" -f $file)
                } catch { $kept.Add($file); Write-Dlss5CoreDebug ("stale temp kept: {0}: {1}" -f $file, $_.Exception.Message) }
            }
        }
        # 4) 事务目录：只清本客户端事务目录里 Write-JsonAtomic 留下的报告临时件。
        foreach ($operation in $operations) {
            $directory = [string]$operation.Directory
            try {
                foreach ($file in @([IO.Directory]::EnumerateFiles($directory, '*', [IO.SearchOption]::TopDirectoryOnly))) {
                    try {
                        if ([IO.File]::GetLastWriteTimeUtc($file) -gt $cutoff) { continue }
                        $matchResult = [regex]::Match([IO.Path]::GetFileName($file), '^(?<base>.+)\.new-[0-9a-fA-F]{32}$')
                        if (-not $matchResult.Success) { continue }
                        $base = $matchResult.Groups['base'].Value
                        if ($base -ne 'transaction.json' -and $base -ne 'rollback.json' -and
                            $base -notmatch '^(restore|repair|uninstall)-[0-9a-f]{32}\.json$') {
                            $kept.Add($file); continue
                        }
                        [IO.File]::Delete($file)
                        $removed.Add($file)
                        Write-Dlss5CoreDebug ("stale temp removed: {0}" -f $file)
                    } catch { $kept.Add($file) }
                }
            } catch { $errors.Add("枚举事务目录 $directory：$($_.Exception.Message)") }
        }
    } catch {
        $errors.Add("清理临时件：$($_.Exception.Message)")
        Write-Dlss5CoreDebug ("stale temp cleanup skipped: {0}" -f $_.Exception.Message)
    }
    return [pscustomobject]@{ Removed = @($removed); Kept = @($kept); Errors = @($errors) }
}

function Invoke-Dlss5CleanupPending {
    # R4：恢复/卸载收尾失败后的幂等重试入口。只删除工具管理的路径：
    # 事务记录 previous_client_record 的写回、按「条目 + 哈希」核对过的备份、空目录、陈旧临时件。
    # 受 ProtectedRoots 与记录校验约束；不认识的备份一律保留。
    # R4（重要）：拿到客户端锁后必须重新读取客户端安装记录——等待锁期间客户端可能已完成新的
    # 安装事务。若活动记录已不属于本事务，立即停止客户端侧的写/删（保留 pending 或标记
    # superseded），绝不覆盖新记录、也不删除新安装的备份。
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Operation, [switch]$Recovery)
    Assert-ProcessesStopped
    $operationFull = Get-CanonicalPath $Operation
    if (-not (Test-Within $operationFull $script:OperationsRoot) -or
        -not ([IO.Path]::GetDirectoryName($operationFull).Equals($script:OperationsRoot, [StringComparison]::OrdinalIgnoreCase))) {
        throw '事务记录不在本 PowerShell 入口的受控目录。'
    }
    Assert-NoReparse $operationFull
    $recordPath = Get-MemberPath $operationFull 'transaction.json'
    if (-not [IO.File]::Exists($recordPath)) { throw '事务记录不存在。' }
    $preview = [IO.File]::ReadAllText($recordPath, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json
    if ($preview.schema -notin @(1, 2) -or [string](Get-ManifestField $preview 'status') -ne 'rolled_back') {
        throw '只有已回滚的事务需要清理。'
    }
    $lock = Get-Dlss5Lock
    $clientFull = Assert-Client ([string](Get-ManifestField $preview 'client')) $lock
    Enter-Dlss5ClientLock $clientFull -Recovery:$Recovery
    try {
        Assert-ProcessesStopped
        # R4：锁内重新读取事务记录——等待锁期间记录可能已被其他入口更新。
        if (-not [IO.File]::Exists($recordPath)) { throw '事务记录不存在。' }
        $record = [IO.File]::ReadAllText($recordPath, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json
        if ($record.schema -notin @(1, 2) -or [string](Get-ManifestField $record 'status') -ne 'rolled_back') {
            throw '只有已回滚的事务需要清理。'
        }
        # R4：旧版本已确认「本事务被后续安装取代」。即使它同时残留 cleanup_pending=true
        # （历史代码的写法），也绝不能再获得客户端写权限：客户端记录可能已被卸载删除，
        # 此时若继续走 previous_client_record 写回分支，就会把更早的安装记录写回已卸载的客户端。
        # 该判断必须先于 cleanup_pending 判断，否则旧记录会被误报为 already_clean 并丢失永久结论。
        $existingCleanupResult = [string](Get-ManifestField $record 'cleanup_result')
        if ($existingCleanupResult -in @('superseded', 'record_absent')) {
            $stalePending = [bool](Get-ManifestField $record 'cleanup_pending')
            if ($stalePending) {
                $record | Add-Member -NotePropertyName cleanup_pending -NotePropertyValue $false -Force
                $record | Add-Member -NotePropertyName cleanup_utc -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force
                Write-JsonAtomic $recordPath $record
            }
            $existingMessage = if ($existingCleanupResult -eq 'record_absent') {
                '客户端安装记录已不存在（可能已卸载），不会再写回更早的记录。'
            } else {
                '该事务的客户端侧清理已被后续安装永久取代，不会再写回旧安装记录。'
            }
            return [pscustomobject]@{ Status = $existingCleanupResult; Operation = $operationFull; RemovedBackups = 0; BackupsKept = @();
                Errors = @(); CleanupIncomplete = $false; CleanupPermanent = $true; CleanupErrors = @();
                SupersededBy = [string](Get-ManifestField $record 'superseded_by');
                Message = $existingMessage }
        }
        if (-not (Get-ManifestField $record 'cleanup_pending')) {
            return [pscustomobject]@{ Status = 'already_clean'; Operation = $operationFull; RemovedBackups = 0; BackupsKept = @();
                Errors = @(); CleanupIncomplete = $false; CleanupPermanent = $false; CleanupErrors = @();
                Message = '该事务没有待完成的清理。' }
        }
        # R4：同一客户端还有别的未完成事务时，暂缓本次清理（保留 cleanup_pending，可稍后重试），
        # 不把它标记成永久失效。
        $otherActive = @(Get-Dlss5Operations -Client $clientFull |
            Where-Object { [string]$_.Status -in @('preparing', 'in_progress') })
        if ($otherActive.Count -gt 0) {
            $record | Add-Member -NotePropertyName cleanup_utc -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force
            Write-JsonAtomic $recordPath $record
            return [pscustomobject]@{ Status = 'deferred'; Operation = $operationFull; RemovedBackups = 0; BackupsKept = @();
                Errors = @(); CleanupIncomplete = $true; CleanupPermanent = $false; CleanupErrors = @();
                SupersededBy = [string]$otherActive[0].Operation;
                Message = '该客户端还有未完成的事务；本次清理暂缓，可在该事务结束后重试。' }
        }
        # rollback_incomplete 的旧事务也意味着客户端可能仍需人工恢复：暂缓而不是永久放弃。
        $otherUnfinished = @(Get-Dlss5Operations -Client $clientFull |
            Where-Object { [string]$_.Status -eq 'rollback_incomplete' })
        if ($otherUnfinished.Count -gt 0) {
            $record | Add-Member -NotePropertyName cleanup_utc -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force
            Write-JsonAtomic $recordPath $record
            return [pscustomobject]@{ Status = 'deferred'; Operation = $operationFull; RemovedBackups = 0; BackupsKept = @();
                Errors = @(); CleanupIncomplete = $true; CleanupPermanent = $false; CleanupErrors = @();
                SupersededBy = [string]$otherUnfinished[0].Operation;
                Message = '该客户端还有回滚未完成的事务；本次清理暂缓，可先恢复该事务后重试。' }
        }
        $clientRecordPath = Get-Dlss5ClientRecordPath $clientFull
        $previousRecord = Get-ManifestField $record 'previous_client_record'
        $currentRecord = Test-Dlss5ClientRecord $clientFull
        if ([IO.File]::Exists($clientRecordPath) -and $null -eq $currentRecord) {
            # 记录文件存在却无法解析：无法证明归属，客户端侧一个字节都不动。
            return [pscustomobject]@{ Status = 'record_unreadable'; Operation = $operationFull; RemovedBackups = 0; BackupsKept = @();
                Errors = @(); CleanupIncomplete = $true; CleanupPermanent = $false; CleanupErrors = @();
                Message = '客户端安装记录无法解析，无法确认归属；已保留现有记录与备份。' }
        }
        $currentOperation = $null
        if ($null -ne $currentRecord) { $currentOperation = [string](Get-ManifestField $currentRecord.Record 'operation') }
        $previousAlreadyRestored = $false
        if ($null -ne $previousRecord -and $null -ne $currentRecord) {
            # previous_client_record 已经原样写回时，这次清理的客户端侧目标已经达成。
            $previousAlreadyRestored =
                (([string](Get-ManifestField $previousRecord 'operation')) -eq $currentOperation) -and
                ((ConvertTo-Json $previousRecord -Compress -Depth 20) -eq (ConvertTo-Json $currentRecord.Record -Compress -Depth 20))
        }
        if ($null -ne $currentRecord -and $currentOperation -ne $operationFull -and -not $previousAlreadyRestored) {
            # R4：客户端已被更新的安装事务接管——只标记旧事务，绝不动新记录/新备份。
            $record | Add-Member -NotePropertyName cleanup_result -NotePropertyValue 'superseded' -Force
            $record | Add-Member -NotePropertyName superseded_by -NotePropertyValue $currentOperation -Force
            $record | Add-Member -NotePropertyName cleanup_pending -NotePropertyValue $false -Force
            $record | Add-Member -NotePropertyName cleanup_utc -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force
            Write-JsonAtomic $recordPath $record
            return [pscustomobject]@{ Status = 'superseded'; Operation = $operationFull; RemovedBackups = 0; BackupsKept = @();
                Errors = @(); CleanupIncomplete = $false; CleanupPermanent = $true; CleanupErrors = @();
                SupersededBy = $currentOperation;
                Message = '客户端已有更新的安装事务；旧事务的客户端侧清理已永久停止，当前安装记录与备份保持不变。' }
        }
        if ($null -ne $previousRecord -and -not $previousAlreadyRestored -and
            -not [IO.File]::Exists($clientRecordPath)) {
            # R4：客户端安装记录已被卸载删除。此时绝不能把 previous_client_record 写回——
            # 那会让已卸载的客户端重新出现更早的安装记录（参见 05 复审 R4）。
            $record | Add-Member -NotePropertyName cleanup_result -NotePropertyValue 'record_absent' -Force
            $record | Add-Member -NotePropertyName cleanup_pending -NotePropertyValue $false -Force
            $record | Add-Member -NotePropertyName cleanup_utc -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force
            Write-JsonAtomic $recordPath $record
            return [pscustomobject]@{ Status = 'record_absent'; Operation = $operationFull; RemovedBackups = 0; BackupsKept = @();
                Errors = @(); CleanupIncomplete = $false; CleanupPermanent = $true; CleanupErrors = @();
                Message = '客户端安装记录已不存在（可能已卸载）；不会再写回更早的安装记录。' }
        }
        $null = Remove-Dlss5StaleTemp $clientFull
        $errors = @()
        $removed = @()
        $kept = @()
        if ($null -ne $previousRecord) {
            if (-not $previousAlreadyRestored) {
                try { Write-JsonAtomic $clientRecordPath $previousRecord }
                catch { $errors += @("写回上一次安装记录：$($_.Exception.Message)") }
            }
        } else {
            $raw = @()
            foreach ($name in @('members', 'entries')) {
                $property = $record.PSObject.Properties[$name]
                if ($null -ne $property -and $null -ne $property.Value) { $raw += @($property.Value) }
            }
            # R11：与卸载/恢复共用「记录登记 + previous_sha256 校验」的逐文件清理，绝不整树删除。
            $cleanup = Remove-Dlss5RecordBackups -ClientFull $clientFull -Entries @($raw | Where-Object { $null -ne $_ })
            $removed = @($cleanup.Removed)
            $kept = @($cleanup.Kept)
            $errors += @($cleanup.Errors)
            try { if ([IO.File]::Exists($clientRecordPath)) { [IO.File]::Delete($clientRecordPath) } }
            catch { $errors += @("删除安装记录：$($_.Exception.Message)") }
        }
        $record | Add-Member -NotePropertyName cleanup_pending -NotePropertyValue (@($errors).Count -gt 0) -Force
        $record | Add-Member -NotePropertyName cleanup_errors -NotePropertyValue @($errors) -Force
        $record | Add-Member -NotePropertyName cleanup_result -NotePropertyValue $(if (@($errors).Count) { 'cleanup_incomplete' } else { 'cleaned' }) -Force
        $record | Add-Member -NotePropertyName cleanup_utc -NotePropertyValue ([DateTime]::UtcNow.ToString('o')) -Force
        Write-JsonAtomic $recordPath $record
        return [pscustomobject]@{ Status = $(if (@($errors).Count) { 'cleanup_incomplete' } else { 'cleaned' });
            Operation = $operationFull; RemovedBackups = @($removed).Count; BackupsKept = @($kept); Errors = @($errors);
            CleanupIncomplete = (@($errors).Count -gt 0); CleanupPermanent = $false; CleanupErrors = @($errors);
            Message = $(if (@($errors).Count) { '清理仍有未完成项，请解除文件占用后重试。' } else { '清理完成。' }) }
    } finally { Exit-Dlss5ClientLock $clientFull }
}


function Get-Dlss5CleanupRetryState {
    # R4（界面）：把 Invoke-Dlss5CleanupPending 的结果映射为结果卡状态。
    # 关键语义：只有「永久失效」（superseded / record_absent）才结束重试；
    # 可恢复的失败（cleanup_incomplete / deferred / record_unreadable）保留重试入口并显示原因。
    [CmdletBinding()]
    param([object]$Previous, [Parameter(Mandatory)][object]$Result)
    $read = {
        param($Object, [string]$Name)
        if ($null -eq $Object) { return $null }
        $property = $Object.PSObject.Properties[$Name]
        if ($null -eq $property) { return $null }
        return $property.Value
    }
    $status = [string](& $read $Result 'Status')
    $resultPermanent = & $read $Result 'Permanent'
    if ($null -eq $resultPermanent) { $resultPermanent = & $read $Result 'CleanupPermanent' }
    $resultIncomplete = & $read $Result 'CleanupIncomplete'
    if ($null -eq $resultIncomplete) {
        $resultIncomplete = $status -in @('cleanup_incomplete', 'deferred', 'record_unreadable')
    }
    $permanent = [bool]$resultPermanent
    $incomplete = [bool]$resultIncomplete
    if ($permanent) { $incomplete = $false }
    # 收集非空错误文本：@($null) 会得到 1 个元素，结果卡会渲染空的「收尾错误：」行。
    $errors = @()
    foreach ($candidate in @((& $read $Result 'CleanupErrors'), (& $read $Result 'Errors'))) {
        foreach ($item in @($candidate)) {
            if ($null -ne $item -and -not [string]::IsNullOrWhiteSpace([string]$item)) { $errors += [string]$item }
        }
    }
    $message = [string](& $read $Result 'CleanupMessage')
    if (-not $message) { $message = [string](& $read $Result 'Message') }
    if (-not $message) {
        $message = switch ($status) {
            'cleaned' { '清理完成。' }
            'already_clean' { '该事务没有待完成的清理。' }
            'cleanup_incomplete' { '清理仍有未完成项，请解除文件占用后重试。' }
            'deferred' { '该客户端还有未完成的事务；本次清理暂缓，可稍后重试。' }
            'superseded' { '该事务的客户端侧清理已被后续安装永久取代，不再重试。' }
            'record_absent' { '客户端安装记录已不存在（可能已卸载），不会再写回更早的记录。' }
            'record_unreadable' { '客户端安装记录无法解析，已保留现状；请先恢复记录。' }
            default { '清理状态未知。' }
        }
    }
    $retryable = $incomplete -and -not $permanent -and
        ($status -in @('cleanup_incomplete', 'deferred', 'record_unreadable'))
    # R4（界面）：SavedModified 必须是 Int32，结果卡直接按整数格式化；旧版用 @(...) 包成
    # 单元素数组后 `[int]` 虽能转换，但 `-gt 0` / 字符串拼接在部分 PowerShell 版本上语义
    # 不稳定，且 StrictMode 下容易暴露成 “Object[]”。这里同时兼容旧对象里已是数组的值。
    $savedModified = 0
    $savedRaw = & $read $Previous 'SavedModified'
    if ($null -ne $savedRaw) {
        if ($savedRaw -is [Array]) {
            if ($savedRaw.Count -eq 1 -and $savedRaw[0] -is [int]) { $savedModified = [int]$savedRaw[0] }
            elseif ($savedRaw.Count -eq 1 -and $null -ne $savedRaw[0]) {
                try { $savedModified = [int]$savedRaw[0] } catch { $savedModified = 0 }
            } else { $savedModified = $savedRaw.Count }
        } else {
            try { $savedModified = [int]$savedRaw } catch { $savedModified = 0 }
        }
    }
    return [pscustomobject]@{
        Status = $status
        Operation = [string](& $read $Result 'Operation')
        SavedModified = $savedModified
        ModifiedRoot = [string](& $read $Previous 'ModifiedRoot')
        Report = & $read $Previous 'Report'
        CleanupIncomplete = $incomplete
        CleanupPermanent = $permanent
        CleanupRetryable = $retryable
        CleanupErrors = @($errors)
        CleanupMessage = $message
        SupersededBy = [string](& $read $Result 'SupersededBy')
        RemovedBackups = [int]((& $read $Result 'RemovedBackups') -as [int])
        BackupsKept = @(& $read $Result 'BackupsKept')
        Errors = @()
    }
}


function Invoke-Dlss5Restore {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Operation, [switch]$Recovery)
    Assert-ProcessesStopped
    $verified = Get-VerifiedOperation $Operation
    Enter-Dlss5ClientLock $verified.Client -Recovery:$Recovery
    try {
        $null = Remove-Dlss5StaleTemp $verified.Client
        $active = Test-Dlss5ClientRecord $verified.Client
        if ($null -ne $active -and [string](Get-ManifestField $active.Record 'operation') -ne $verified.Directory) {
            throw '客户端有后续安装事务，请先恢复最近一次安装。'
        }
        $runId = [guid]::NewGuid().ToString('N')
        $reportPath = Get-MemberPath $verified.Directory ("restore-$runId.json")
        $saveRoot = Get-MemberPath $verified.Directory ("modified-$runId")
        $report = Invoke-Dlss5Teardown -ClientFull $verified.Client -Entries @($verified.Entries) `
            -BackupRoot $verified.BackupRoot -SaveRoot $saveRoot -ReportPath $reportPath
        $cleanupErrors = @()
        $cleanupPending = $false
        $backupsRemoved = 0
        $backupsKept = @()
        if ($report.status -eq 'rolled_back') {
            $verified.Record.status = 'rolled_back'
            $verified.Record | Add-Member -NotePropertyName restore_record -NotePropertyValue $reportPath -Force
            # R4：清理结果必须回写事务记录，供 Invoke-Dlss5CleanupPending 幂等重试。
            $verified.Record | Add-Member -NotePropertyName cleanup_pending -NotePropertyValue $true -Force
            Write-JsonAtomic $verified.RecordPath $verified.Record
            # zh: 恢复成功后同步清掉客户端内安装记录与原件备份——否则「已恢复」的客户端
            # 仍会被 Get-Dlss5Status 报成「已安装」，卸载也会按过期记录再跑一遍（20261112）。
            try {
                $clientRecord = Get-Dlss5ClientRecordPath $verified.Client
                $previousRecord = Get-ManifestField $verified.Record 'previous_client_record'
                if ($null -ne $previousRecord) {
                    Write-JsonAtomic $clientRecord $previousRecord
                } else {
                    # R11：与卸载/清理共用「记录登记 + previous_sha256 校验」的逐文件清理；
                    # 未登记文件与哈希不符的备份一律保留，最后只删空目录——不再整树删除。
                    $cleanup = Remove-Dlss5RecordBackups -ClientFull $verified.Client -Entries @($verified.Entries)
                    $backupsRemoved = @($cleanup.Removed).Count
                    $backupsKept = @($cleanup.Kept)
                    $cleanupErrors += @($cleanup.Errors)
                    if (@($cleanup.Errors).Count -eq 0 -and [IO.File]::Exists($clientRecord)) { [IO.File]::Delete($clientRecord) }
                }
            } catch {
                $cleanupErrors += @("清理客户端内安装记录：$($_.Exception.Message)")
            }
            if (@($cleanupErrors).Count) { $cleanupPending = $true }
            $verified.Record | Add-Member -NotePropertyName cleanup_pending -NotePropertyValue $cleanupPending -Force
            $verified.Record | Add-Member -NotePropertyName cleanup_errors -NotePropertyValue @($cleanupErrors) -Force
            Write-JsonAtomic $verified.RecordPath $verified.Record
        }
        return [pscustomobject]@{ Status = $report.status; Report = $reportPath;
            Operation = $verified.Directory; ModifiedRoot = $saveRoot;
            SavedModified = @($report.saved_modified).Count; Errors = @($report.errors);
            CleanupIncomplete = $cleanupPending; CleanupErrors = @($cleanupErrors);
            BackupsRemoved = $backupsRemoved; BackupsKept = @($backupsKept) }
    } finally { Exit-Dlss5ClientLock $verified.Client }
}


function Invoke-Dlss5Uninstall {
    # zh: 卸载读客户端内的安装记录（host64\dlss5-install-record.json），不依赖 _scratch 事务记录。
    # 按记录逐项：安装前存在 → 还原原件；安装前不存在 → 删除新增文件；然后清运行产物。
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Client, [switch]$KeepRecord, [switch]$Recovery)
    Assert-ProcessesStopped
    $lock = Get-Dlss5Lock
    $trustedManifest = Get-Dlss5Manifest
    $clientFull = Assert-Client $Client $lock
    Enter-Dlss5ClientLock $clientFull -Recovery:$Recovery
    try {
        $null = Remove-Dlss5StaleTemp $clientFull
        $recordInfo = Test-Dlss5ClientRecord $clientFull
        if ($null -eq $recordInfo) {
            throw '客户端内没有安装记录（host64\dlss5-install-record.json），无法按清单卸载。若还保留 _scratch 事务记录，可改用「恢复安装前文件」。'
        }
        $warnings = @()
        $unknownTargets = @($recordInfo.UnknownTargets)
        if (@($unknownTargets).Count) {
            $warnings += ('安装记录含当前组件锁无法识别的条目（{0} 项），本次跳过并保留：{1}' -f
                @($unknownTargets).Count, (@($unknownTargets) -join '、'))
        }
        $entries = @(@($recordInfo.Record.entries) | Where-Object { $unknownTargets -notcontains [string]$_.target } |
            ForEach-Object { [ordered]@{ target = [string]$_.target; sha256 = [string]$_.sha256;
                previous_sha256 = $_.previous_sha256; pre_state = [string]$_.pre_state; backup = $_.backup } })
        if ($entries.Count -eq 0) {
            throw '安装记录里的文件都不在当前组件锁中（记录可能来自另一版本组件），无法按清单卸载。请改用「恢复安装前文件」或手动处理。'
        }
        $runId = [guid]::NewGuid().ToString('N')
        Assert-NoReparse $script:OperationsRoot
        [IO.Directory]::CreateDirectory($script:OperationsRoot) | Out-Null
        $reportPath = Join-Path $script:OperationsRoot ("uninstall-$runId.json")
        $saveRoot = Join-Path $script:OperationsRoot ("uninstall-modified-$runId")
        $report = Invoke-Dlss5Teardown -ClientFull $clientFull -Entries $entries `
            -BackupRoot $recordInfo.BackupRoot -SaveRoot $saveRoot -ReportPath $reportPath
        $removed = @(); $reported = @()
        if ($report.status -eq 'rolled_back') {
            foreach ($item in @($recordInfo.Record.residue)) {
                $rel = [string](Get-ManifestField $item 'path')
                if (-not $rel) { continue }
                $kind = [string](Get-ManifestField $item 'kind')
                $trusted = @($trustedManifest.Residue | Where-Object {
                    [string](Get-ManifestField $_ 'path') -eq $rel -and
                    [string](Get-ManifestField $_ 'kind') -eq $kind -and
                    [string](Get-ManifestField $_ 'policy') -eq 'delete'
                })
                if ([string](Get-ManifestField $item 'policy') -ne 'delete' -or $trusted.Count -ne 1) {
                    $reported += $rel; continue
                }
                if ($rel -like '%*%') { $reported += $rel; continue }
                try {
                    $path = Get-MemberPath $clientFull $rel
                    if ([IO.Directory]::Exists($path)) {
                        if ($kind -ne 'tree') { throw '残留类型变化，保留现状。' }
                        $pending = [Collections.Generic.Stack[string]]::new()
                        $pending.Push($path)
                        while ($pending.Count) {
                            $directory = $pending.Pop()
                            Assert-NoReparse $directory
                            foreach ($child in Get-ChildItem -LiteralPath $directory -Force -ErrorAction Stop) {
                                if ($child.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw '残留目录包含链接，保留现状。' }
                                if ($child.PSIsContainer) { $pending.Push($child.FullName) }
                            }
                        }
                        Assert-NoReparse $path
                        [IO.Directory]::Delete($path, $true); $removed += $rel
                    } elseif ([IO.File]::Exists($path)) {
                        if ($kind -ne 'file') { throw '残留类型变化，保留现状。' }
                        [IO.File]::Delete($path); $removed += $rel
                    }
                } catch { $report.errors = @($report.errors) + @("残留 $rel：$($_.Exception.Message)") }
            }
        }
        # R11：与恢复/Invoke-Dlss5CleanupPending 共用「记录登记 + previous_sha256 校验」的逐文件清理：
        # 只删记录登记且哈希仍等于 previous_sha256 的备份；未登记文件、内容变化的备份一律保留并报告。
        # -KeepRecord 表示保留记录供诊断/续卸，此时备份也必须一并保留，否则记录失去依据。
        $backupRemoved = 0
        $backupKept = @()
        $cleanupErrors = @()
        if ($report.status -eq 'rolled_back' -and -not $KeepRecord) {
            if (@($report.errors).Count) {
                $cleanupErrors += @($report.errors)
            } else {
                $registeredEntries = @(@($recordInfo.Record.entries) | Where-Object { $unknownTargets -notcontains [string]$_.target })
                $cleanup = Remove-Dlss5RecordBackups -ClientFull $clientFull -Entries $registeredEntries
                $backupRemoved = @($cleanup.Removed).Count
                $backupKept = @($cleanup.Kept)
                $cleanupErrors += @($cleanup.Errors)
            }
        }
        if (-not $KeepRecord) {
            if (@($report.errors).Count -or @($cleanupErrors).Count) {
                Write-Dlss5CoreDebug 'uninstall cleanup incomplete: keeping the install record and backups for retry'
            } else {
                try { if ([IO.File]::Exists($recordInfo.Path)) { [IO.File]::Delete($recordInfo.Path) } }
                catch { $cleanupErrors += @("删除安装记录：$($_.Exception.Message)") }
            }
        }
        $allErrors = @($report.errors) + @($cleanupErrors)
        # 回滚/清理任一未完成都意味着记录与备份仍被保留，需要用户重试。
        $cleanupIncomplete = (@($report.errors).Count -gt 0 -or @($cleanupErrors).Count -gt 0)
        $status = if (@($allErrors).Count) { 'rollback_incomplete' } else { 'uninstalled' }
        $summary = [ordered]@{ status = $status; client = $clientFull; restored = @($report.restored);
            deleted = @($report.deleted); saved_modified = @($report.saved_modified);
            residue_removed = @($removed); residue_reported = @($reported);
            backups_removed = $backupRemoved; backups_kept = @($backupKept);
            cleanup_incomplete = $cleanupIncomplete; warnings = @($warnings);
            errors = @($allErrors); report = $reportPath }
        Write-JsonAtomic $reportPath $summary
        return [pscustomobject]@{ Status = $status; Report = $reportPath; ModifiedRoot = $saveRoot;
            Restored = @($report.restored).Count; Deleted = @($report.deleted).Count;
            SavedModified = @($report.saved_modified).Count; ResidueRemoved = @($removed).Count;
            ResidueReported = @($reported); Errors = @($allErrors);
            CleanupIncomplete = $cleanupIncomplete; CleanupErrors = @($cleanupErrors);
            BackupsRemoved = $backupRemoved; BackupsKept = @($backupKept); Warnings = @($warnings) }
    } finally { Exit-Dlss5ClientLock $clientFull }
}


function Invoke-Dlss5Repair {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Operation, [switch]$AdoptUninstalled)
    Assert-ProcessesStopped
    $verified = Get-VerifiedOperation $Operation
    if ($verified.Record.status -ne 'completed') { throw '未完成的安装请先恢复，不能直接修复。' }
    Enter-Dlss5ClientLock $verified.Client
    try {
        Assert-ProcessesStopped
        $null = Remove-Dlss5StaleTemp $verified.Client
        $adopt = [bool]$AdoptUninstalled
        $warnings = @()
        $clientRecordPath = Get-Dlss5ClientRecordPath $verified.Client
        $clientRecord = Test-Dlss5ClientRecord $verified.Client
        if ($null -ne $clientRecord) {
            $recordOperation = [string](Get-ManifestField $clientRecord.Record 'operation')
            if ($recordOperation -ne $verified.Directory) {
                throw ("客户端安装记录属于另一个安装事务（{0}），请对最近一次安装执行修复，或先恢复最近一次安装。" -f $recordOperation)
            }
            if (@($clientRecord.UnknownTargets).Count) {
                $warnings += ('客户端安装记录含当前组件锁无法识别的条目（{0} 项），将原样保留：{1}' -f
                    @($clientRecord.UnknownTargets).Count, (@($clientRecord.UnknownTargets) -join '、'))
            }
        } else {
            if ([IO.File]::Exists($clientRecordPath)) {
                # R1：记录文件损坏时，普通修复必须保留现状并拒绝；显式 -AdoptUninstalled 允许按本次
                # 事务重建记录（前置检查仍会证明每个 pre_state=present 的原件备份完好），失败时不改动该文件。
                if (-not $adopt) {
                    throw '客户端安装记录不可读，保留现状；请先恢复该记录或使用重新安装。'
                }
                $warnings += '客户端安装记录不可读，已按 -AdoptUninstalled 采用本次事务重建安装记录（失败时保持原样）。'
            } elseif (-not $adopt) {
                # R1：没有客户端记录时默认拒绝，避免修复半途而废；确需恢复登记必须显式采用本次事务。
                throw '客户端内没有安装记录。要重新登记为已安装，请使用「重新安装」，或显式使用 -AdoptUninstalled 采用本次事务的成员重建记录。'
            }
        }
        $runId = [guid]::NewGuid().ToString('N')
        $reportPath = Get-MemberPath $verified.Directory ("repair-$runId.json")
        $conflictRoot = Get-MemberPath $verified.Directory ("repair-modified-$runId")
        $report = [ordered]@{ status = 'in_progress'; repaired = @(); preserved = @();
            adopted = $false; warnings = @($warnings); errors = @(); preflight = @() }
        # Repair only recorded installed members; missing trusted inputs are errors.
        $repairEntries = Get-ManifestField $verified.Record 'repair_entries'
        if ($null -eq $repairEntries) { $repairEntries = @($verified.Entries) }
        $repairEntries = @($repairEntries | Where-Object { $null -ne $_ })
        $backupRoot = Get-MemberPath $verified.Client 'host64/dlss5-backup'
        $preserveTargets = @('host64/ReShade.ini', 'dlss5-aio-x86.cfg', 'ReShade.ini', 'host64/ReShadePreset.ini')
        # ---- R1：所有可提前判定的条件都在第一次客户端写入之前检查 ----
        # 修复来源、目标位置/可写性，以及 adopt 所需的原件备份身份都必须先成立；任何一项失败时
        # 只写事务目录内的报告，客户端文件与客户端安装记录保持不变。
        $problems = @()
        $plan = [Collections.Generic.List[object]]::new()
        $plannedTargets = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($installedEntry in $repairEntries) {
            $target = [string](Get-ManifestField $installedEntry 'target')
            $expected = [string](Get-ManifestField $installedEntry 'sha256')
            if (-not $target) { continue }
            if (-not $plannedTargets.Add($target)) { continue }
            try {
                $matches = @($verified.Lock.members | Where-Object { $_.target -eq $target -and $_.sha256 -eq $expected })
                if ($matches.Count -ne 1) { $problems += @("$target：修复记录与组件锁不一致"); continue }
                $member = $matches[0]
                $dest = Get-MemberPath $verified.Client $target
                if ([IO.Directory]::Exists($dest)) { $problems += @("$target：目标位置是目录，保留现状"); continue }
                $exists = [IO.File]::Exists($dest)
                $current = if ($exists) { Get-Sha256 $dest } else { $null }
                $preserved = $exists -and ($target -in $preserveTargets)
                $needsWrite = ($current -ne $expected) -and -not $preserved
                if ($needsWrite) {
                    $staged = Get-MemberPath $verified.Directory ('stage/' + $target)
                    if (-not [IO.File]::Exists($staged) -or (Get-Sha256 $staged) -ne $expected) {
                        $problems += @("$target：修复来源缺失或身份不符，请重新提供组件"); continue
                    }
                }
                if ($exists) {
                    try {
                        $probe = [IO.File]::Open($dest, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite)
                        $probe.Dispose()
                    } catch { $problems += @("$target：目标文件不可写（$($_.Exception.Message)）"); continue }
                } else {
                    $parent = [IO.Path]::GetDirectoryName($dest)
                    while ($parent -and -not [IO.Directory]::Exists($parent)) { $parent = [IO.Path]::GetDirectoryName($parent) }
                    if (-not $parent) { $problems += @("$target：目标目录无法创建"); continue }
                    try { Assert-NoReparse $parent } catch { $problems += @("$target：目标目录不可用（$($_.Exception.Message)）"); continue }
                }
                $plan.Add([pscustomobject]@{ Target = $target; Dest = $dest; Sha256 = $expected;
                    Exists = $exists; Current = $current; Preserved = $preserved; NeedsWrite = $needsWrite })
            } catch { $problems += @("$target：$($_.Exception.Message)") }
        }
        if ($adopt) {
            # adopt：除修复来源外还必须能证明原件备份仍在（未被恢复/清理），否则说明客户端已经
            # 完成过恢复，绝不能把它重新登记成「已安装」。
            foreach ($entry in $repairEntries) {
                $target = [string](Get-ManifestField $entry 'target')
                if (-not $target) { continue }
                $expected = [string](Get-ManifestField $entry 'sha256')
                $lockMatch = @($verified.Lock.members | Where-Object { [string]$_.target -eq $target -and [string]$_.sha256 -eq $expected })
                if ($lockMatch.Count -ne 1) { continue }
                $planItem = @($plan | Where-Object { $_.Target -eq $target })
                $preserved = @($planItem).Count -eq 1 -and [bool]$planItem[0].Preserved
                if ([string](Get-ManifestField $entry 'pre_state') -eq 'present' -and -not $preserved) {
                    $previous = [string](Get-ManifestField $entry 'previous_sha256')
                    $backupRel = [string](Get-ManifestField $entry 'backup')
                    if (-not $backupRel) { $problems += @("$target：缺少原件备份路径"); continue }
                    $backupPath = Get-MemberPath $backupRoot $backupRel
                    if (-not [IO.File]::Exists($backupPath)) { $problems += @("$target：原件备份缺失（客户端可能已恢复，不能接管）"); continue }
                    if (-not $previous) { $problems += @("$target：缺少原件备份哈希"); continue }
                    if ((Get-Sha256 $backupPath) -ne $previous) { $problems += @("$target：原件备份身份不符"); continue }
                }
            }
        }
        if (@($problems).Count) {
            $report.status = 'repair_incomplete'
            $report.preflight = @($problems)
            $report.errors = @($problems)
            Write-JsonAtomic $reportPath $report
            $detail = '修复前置检查未通过（未写入任何客户端文件，客户端安装记录未改变）：' + (@($problems) -join '；')
            # adopt 必须明确失败（不能把已恢复的客户端重新登记成已安装）；普通修复保持
            # 「返回 repair_incomplete + Errors」的既有契约，由调用方决定如何提示。
            if ($adopt) { throw $detail }
            return [pscustomobject]@{ Status = 'repair_incomplete'; Report = $reportPath; Repaired = 0;
                Preserved = @(); Errors = @($problems); Adopted = $false; Warnings = @($warnings) }
        }
        Write-JsonAtomic $reportPath $report
        $journal = [Collections.Generic.List[object]]::new()
        foreach ($item in $plan) {
            $dest = [string]$item.Dest
            $temp = "$dest.dlss5-repair-$runId"
            $saved = $null
            try {
                Assert-ProcessesStopped
                if ($item.Current -eq $item.Sha256) { continue }
                if ($item.Preserved) { $report.preserved = @($report.preserved) + @($item.Target); continue }
                if ($item.Exists) {
                    # 先保存原件副本，供回滚使用；此时目标尚未被写入，失败不影响客户端文件。
                    $saved = Get-MemberPath $conflictRoot ([string]$item.Target)
                    Copy-Verified $dest $saved ([string]$item.Current)
                }
                [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dest)) | Out-Null
                Assert-NoReparse $dest
                $staged = Get-MemberPath $verified.Directory ('stage/' + [string]$item.Target)
                Copy-Verified $staged $temp ([string]$item.Sha256)
                if ($item.Exists) {
                    if ((Get-Sha256 $dest) -ne [string]$item.Current) { throw '文件在修复期间变化，保留现状。' }
                    Move-AtomicReplace $temp $dest
                } else {
                    # R1：目标可能在预检后被其他程序创建。Move 失败时保留对方文件，本次不记账、不回滚。
                    [IO.File]::Move($temp, $dest)
                }
                # R1：只有目标确实被本次写入之后才登记回滚条目；复制临时文件或 Move 失败的目标
                # 不得进入回滚清单，否则回滚会把它误判成「修复后被其他程序修改」。
                if ($item.Exists) {
                    $journal.Add([pscustomobject]@{ Target = [string]$item.Target; Action = 'replace';
                        Backup = $saved; Sha256 = [string]$item.Sha256; previous_sha256 = [string]$item.Current })
                } else {
                    $journal.Add([pscustomobject]@{ Target = [string]$item.Target; Action = 'create';
                        Backup = $null; Sha256 = [string]$item.Sha256; previous_sha256 = $null })
                }
                if ((Get-Sha256 $dest) -ne [string]$item.Sha256) { throw '修复回读失败。' }
                $report.repaired = @($report.repaired) + @([string]$item.Target)
            } catch {
                # 只清理本次临时件；未写入成功的目标不在 journal 内，不会被回滚逻辑误处理。
                try { if ([IO.File]::Exists($temp)) { [IO.File]::Delete($temp) } } catch { }
                $report.errors = @($report.errors) + @("$($item.Target)：$($_.Exception.Message)")
            }
        }
        if (@($report.errors).Count) {
            # R1：执行中失败必须回滚已写入的文件，并保持客户端安装记录不变。
            $undo = Undo-Dlss5RepairWrites -ClientFull $verified.Client -Journal @($journal) -RunId $runId
            $report.status = 'repair_incomplete'
            $report | Add-Member -NotePropertyName rolled_back -NotePropertyValue ([int]$undo.RolledBack) -Force
            $report | Add-Member -NotePropertyName rollback_errors -NotePropertyValue @($undo.Errors) -Force
            Write-JsonAtomic $reportPath $report
            $message = '修复未完成，已回滚本次写入的 {0} 个文件；客户端安装记录未改变。' -f $undo.RolledBack
            if (@($undo.Errors).Count) {
                throw ($message + ' 回滚未完全成功：' + ((@($undo.Errors) + @($report.errors)) -join '；'))
            }
            throw ($message + ' 错误：' + (@($report.errors) -join '；'))
        }
        $report.status = 'completed'
        $adopted = $false
        if ($adopt) {
            $adoptEntries = $repairEntries
            $manifestVersion = [string](Get-ManifestField $verified.Record 'manifest_version')
            $manifest = Get-Dlss5Manifest
            $newRecord = [ordered]@{ schema = 1; status = 'installed';
                installed_utc = [DateTime]::UtcNow.ToString('o'); client = $verified.Client;
                manifest_version = $manifestVersion; tool = 'WOW-335-NR'; operation = $verified.Directory;
                entries = @($adoptEntries); residue = @($manifest.Residue) }
            try {
                Write-JsonAtomic $clientRecordPath $newRecord
            } catch {
                $undo = Undo-Dlss5RepairWrites -ClientFull $verified.Client -Journal @($journal) -RunId $runId
                $report.status = 'repair_incomplete'
                $report.errors = @($report.errors) + @("写入安装记录：$($_.Exception.Message)")
                $report | Add-Member -NotePropertyName rolled_back -NotePropertyValue ([int]$undo.RolledBack) -Force
                $report | Add-Member -NotePropertyName rollback_errors -NotePropertyValue @($undo.Errors) -Force
                Write-JsonAtomic $reportPath $report
                throw ('无法写入客户端安装记录（已回滚本次写入的 {0} 个文件）：{1}' -f $undo.RolledBack, $_.Exception.Message)
            }
            $adopted = $true
            $report.adopted = $true
            $report | Add-Member -NotePropertyName record -NotePropertyValue $clientRecordPath -Force
        }
        Write-JsonAtomic $reportPath $report
        return [pscustomobject]@{ Status = $report.status; Report = $reportPath; Repaired = @($report.repaired).Count;
            Preserved = @($report.preserved); Errors = @($report.errors);
            Adopted = $adopted; Warnings = @($warnings) }
    } finally { Exit-Dlss5ClientLock $verified.Client }
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
                InputWidth = [BitConverter]::ToUInt32($bytes, 56); InputHeight = [BitConverter]::ToUInt32($bytes, 60);
                OutputWidth = [BitConverter]::ToUInt32($bytes, 64); OutputHeight = [BitConverter]::ToUInt32($bytes, 68);
                SourceFps = [BitConverter]::ToUInt32($bytes, 72); ProxyFps = [BitConverter]::ToUInt32($bytes, 76);
                ActiveNrModel = [BitConverter]::ToUInt32($bytes, 80);
                GpuTotalUs = [BitConverter]::ToUInt32($bytes, 116);
                SourceFrameAverageUs = [BitConverter]::ToUInt32($bytes, 152);
                NrFrames = [BitConverter]::ToUInt64($bytes, 200); SrFrames = [BitConverter]::ToUInt64($bytes, 208);
                FgFrames = [BitConverter]::ToUInt64($bytes, 216); FramesPresented = [BitConverter]::ToUInt64($bytes, 224) }
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
    $sw = [Diagnostics.Stopwatch]::StartNew()
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
    Write-Dlss5CoreDebug ('perf: identity ' + $sw.ElapsedMilliseconds + ' ms')
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
    Write-Dlss5CoreDebug ('perf: config ' + $sw.ElapsedMilliseconds + ' ms')
    # zh: 进程枚举改用 Get-Process（Win32_Process 的 CIM 查询在本机约 0.7 秒，
    # 每次切页都会触发，是卡顿主因）。字段口径保持：仅统计与本客户端路径一致的游戏/载体进程。
    $gamePath = Get-MemberPath $clientFull 'wow.exe'
    $hostPath = Get-MemberPath $clientFull 'host64/AIO DLSS5 32-bit Wrapper.exe'
    $games = @()
    $hosts = @()
    $unknown = 0
    try {
        foreach ($item in @(Get-Process -Name 'wow' -ErrorAction SilentlyContinue)) {
            try {
                if ($item.Path) { if ($item.Path.Equals($gamePath, [StringComparison]::OrdinalIgnoreCase)) { $games += $item } }
                else { $unknown++ }
            } catch { $unknown++ }
        }
        foreach ($item in @(Get-Process -Name 'AIO DLSS5 32-bit Wrapper' -ErrorAction SilentlyContinue)) {
            try {
                if ($item.Path) { if ($item.Path.Equals($hostPath, [StringComparison]::OrdinalIgnoreCase)) { $hosts += $item } }
                else { $unknown++ }
            } catch { $unknown++ }
        }
    } catch { }
    $telemetry = if ($hosts.Count -eq 1 -and $unknown -eq 0) { Read-AioTelemetry ([int]$hosts[0].Id) }
                 else { [pscustomobject]@{ Available = $false; Reason = '载体未运行、数量不唯一或身份不可读' } }
    Write-Dlss5CoreDebug ('perf: procs+telemetry ' + $sw.ElapsedMilliseconds + ' ms')
    $now = [DateTime]::UtcNow
    if ($null -eq $script:GpuCache -or ($now - $script:GpuCacheAt).TotalSeconds -gt 60) {
        $script:GpuCache = @(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object {
            [pscustomobject]@{ Name = $_.Name; DriverVersion = $_.DriverVersion }
        })
        $script:GpuCacheAt = $now
    }
    $graphics = $script:GpuCache
    Write-Dlss5CoreDebug ('perf: gpu ' + $sw.ElapsedMilliseconds + ' ms')
    $recordInfo = Test-Dlss5ClientRecord $clientFull
    $unknownTargets = if ($null -ne $recordInfo) { @($recordInfo.UnknownTargets) } else { @() }
    $installRecord = if ($null -eq $recordInfo) { '没有（首次安装或已卸载）' }
                     elseif (@($unknownTargets).Count) {
                         "存在（$(@($recordInfo.Record.entries).Count) 项，$($recordInfo.Record.installed_utc)；其中 $(@($unknownTargets).Count) 项不在当前组件锁中，卸载时会保留）"
                     }
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
    Get-Dlss5Manifest, Export-Dlss5ManifestList, Test-Dlss5ClientRecord, Get-Sha256,
    Invoke-Dlss5CleanupPending, Get-Dlss5CleanupRetryState
