Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output = Join-Path ([IO.Path]::GetFullPath((Join-Path $repo '../..'))) ('_scratch/dlss5-aio/record-safety-' + [guid]::NewGuid().ToString('N'))
$current = $output
while ($current) {
    $item = Get-Item -LiteralPath $current -Force -ErrorAction SilentlyContinue
    if ($null -ne $item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Test path contains a link.' }
    $current = [IO.Path]::GetDirectoryName($current)
}
$client = Join-Path $output 'client'
foreach ($path in @($output, $client, (Join-Path $client 'host64'), (Join-Path $client 'Data'), (Join-Path $client 'WTF'))) {
    [IO.Directory]::CreateDirectory($path) | Out-Null
}
$encoding = [Text.UTF8Encoding]::new($false)
$pe = [byte[]]::new(512)
$pe[0]=0x4d; $pe[1]=0x5a; $pe[0x3c]=0x80; $pe[0x80]=0x50; $pe[0x81]=0x45; $pe[0x84]=0x4c; $pe[0x85]=1
[IO.File]::WriteAllBytes((Join-Path $client 'wow.exe'), $pe)
$module = Import-Module (Join-Path $repo 'Dlss5.Core.psm1') -Force -PassThru -WarningAction SilentlyContinue
& $module {
    param($root)
    $script:OperationsRoot = Join-Path $root 'operations'
    $script:ProtectedRoots = @()
    function script:Assert-ProcessesStopped {}
} $output
$lock = Get-Content (Join-Path $repo 'payload-lock.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$member = @($lock.members | Where-Object { $_.target -eq 'd3d9.dll' })[0]
$recordPath = Join-Path $client 'host64/dlss5-install-record.json'
function New-Record {
    return [ordered]@{schema=1;status='installed';entries=@([ordered]@{target='d3d9.dll';sha256=$member.sha256;pre_state='absent';backup=$null;previous_sha256=$null;replaces_original=$true});residue=@()}
}
function Save-Record($record) { [IO.File]::WriteAllText($recordPath, ($record | ConvertTo-Json -Depth 10), $encoding) }
function Check([bool]$ok, [string]$message) { if (-not $ok) { throw $message } }
# Neither a new path nor a changed policy/type can authorize deletion.
$record = New-Record
foreach ($relative in @('Data/sentinel.txt', 'WTF/sentinel.txt', 'unrelated.txt', 'host64/live-params.json')) {
    [IO.File]::WriteAllText((Join-Path $client $relative), 'keep', $encoding)
    $record.residue += [ordered]@{path=$relative;kind='tree';policy='delete'}
}
Save-Record $record
$result = Invoke-Dlss5Uninstall -Client $client
Check ($result.Status -eq 'uninstalled' -and $result.ResidueRemoved -eq 0) 'Untrusted residue was deleted.'
foreach ($relative in @('Data/sentinel.txt', 'WTF/sentinel.txt', 'unrelated.txt', 'host64/live-params.json')) {
    Check ([IO.File]::Exists((Join-Path $client $relative))) ('Lost untrusted file: ' + $relative)
}
$record = New-Record
$record.residue = @([ordered]@{path='host64/live-params.json';kind='file';policy='delete'})
Save-Record $record
$result = Invoke-Dlss5Uninstall -Client $client
Check ($result.Status -eq 'uninstalled' -and -not [IO.File]::Exists((Join-Path $client 'host64/live-params.json'))) 'Trusted residue not removed.'
foreach ($variation in @('target', 'pre_state', 'sha256', 'backup', 'duplicate')) {
    $record = New-Record
    switch ($variation) {
        'target' { $record.entries[0].target = 'Data/sentinel.txt' }
        'pre_state' { $record.entries[0].pre_state = 'invalid' }
        'sha256' { $record.entries[0].sha256 = 'invalid' }
        'backup' { $record.entries[0].backup = 'Data/sentinel.txt' }
        'duplicate' { $record.entries += $record.entries[0] }
    }
    Save-Record $record
    $refused = $false
    try { $null = Invoke-Dlss5Uninstall -Client $client } catch { $refused = $true }
    Check ($refused -and [IO.File]::Exists($recordPath) -and [IO.File]::Exists((Join-Path $client 'Data/sentinel.txt'))) ('Invalid entry accepted: ' + $variation)
}
# A trusted directory containing a junction must be preserved, along with its record.
$outside = Join-Path $output 'outside'
[IO.Directory]::CreateDirectory($outside) | Out-Null
[IO.File]::WriteAllText((Join-Path $outside 'sentinel.txt'), 'keep', $encoding)
$logs = Join-Path $client 'host64/panel-logs'
[IO.Directory]::CreateDirectory($logs) | Out-Null
New-Item -ItemType Junction -Path (Join-Path $logs 'linked') -Target $outside | Out-Null
$record = New-Record
$record.residue = @([ordered]@{path='host64/panel-logs';kind='tree';policy='delete'})
Save-Record $record
$result = Invoke-Dlss5Uninstall -Client $client
Check ($result.Status -eq 'rollback_incomplete' -and [IO.File]::Exists($recordPath) -and [IO.File]::Exists((Join-Path $outside 'sentinel.txt'))) 'Linked residue was followed or recovery record removed.'
Write-Output "Record safety passed: $output"
