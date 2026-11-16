param(
    [string]$Client,
    [string]$Stage,
    [string]$Watcher,
    [switch]$Smoke,
    [string]$SmokeView,
    [int]$AutoCloseSeconds = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

# zh: DPI 感知必须在创建任何窗口之前声明。未声明时 Windows 把本进程当成「DPI 不敏感」，
# 系统缩放到 125%/150% 时由 DWM 拉伸整窗口位图，文字全部插值变糊（20261113 用户报告）。
if (-not ('Wow335Ui.Dpi' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Wow335Ui {
    public static class Dpi {
        [DllImport("user32.dll", SetLastError = true)] static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
        [DllImport("user32.dll", SetLastError = true)] static extern bool SetProcessDPIAware();
        [DllImport("user32.dll")] static extern uint GetDpiForSystem();
        [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
        [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr w, IntPtr h);
        [DllImport("gdi32.dll")] static extern int GetDeviceCaps(IntPtr dc, int index);
        public static bool Aware = false;
        public static double Scale = 1.0;
        public static void Enable() {
            try { if (SetProcessDpiAwarenessContext(new IntPtr(-4))) Aware = true; } catch { }
            if (!Aware) { try { if (SetProcessDPIAware()) Aware = true; } catch { } }
            uint dpi = 96;
            if (Aware) { try { dpi = GetDpiForSystem(); } catch { dpi = 96; } }
            if (dpi == 0 || dpi == 96 && !Aware) { dpi = 96; }
            Scale = dpi / 96.0;
            if (Scale < 0.5 || Scale > 4.0) Scale = 1.0;
        }
    }
}
'@
}
if (-not ('Wow335Ui.DragNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Wow335Ui {
    public static class DragNative {
        [DllImport("user32.dll")] public static extern bool ReleaseCapture();
        [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, int msg, IntPtr w, IntPtr l);
        public static void StartMove(IntPtr form) {
            ReleaseCapture();
            SendMessage(form, 0xA1, (IntPtr)2, IntPtr.Zero);
        }
    }
}
'@ -ReferencedAssemblies @([Windows.Forms.NativeWindow].Assembly.Location, [Windows.Forms.Message].Assembly.Location)
}
if (-not ('Wow335Ui.CaptionHook' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Windows.Forms;
namespace Wow335Ui {
    public struct Rect { public int Left, Top, Right, Bottom; }
    public class CaptionHook : NativeWindow {
        [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out Rect r);
        public bool Transparent = false;
        public int CaptionHeight = 0;
        public IntPtr FormHandle = IntPtr.Zero;
        protected override void WndProc(ref Message m) {
            if (m.Msg == 0x84) {
                if (Transparent) { m.Result = (IntPtr)(-1); return; }
                if (m.HWnd == FormHandle) {
                    int x = (short)((long)m.LParam & 0xFFFF);
                    int y = (short)(((long)m.LParam >> 16) & 0xFFFF);
                    Rect r;
                    if (GetWindowRect(m.HWnd, out r) && x >= r.Left && x < r.Right && y >= r.Top && y < r.Top + CaptionHeight) {
                        m.Result = (IntPtr)2;
                        return;
                    }
                }
            }
            base.WndProc(ref m);
        }
    }
}
'@ -ReferencedAssemblies @([Windows.Forms.NativeWindow].Assembly.Location, [Windows.Forms.Message].Assembly.Location)
}
[Wow335Ui.Dpi]::Enable()
$script:uiScale = [double][Wow335Ui.Dpi]::Scale
$script:uiDpiAware = [bool][Wow335Ui.Dpi]::Aware

# zh: 布局常量全部按 96dpi 逻辑像素写，U() 在创建控件时一次换算成物理像素。
# 旧版是「先按逻辑像素建好、再遍历控件树缩放」，位置与字体换算不同步，
# DPI 125% 下行内间距不足、文字互相压住（20261113 实测截图）。
function U([double]$Value) {
    if ($script:uiScale -eq 1.0) { return [int][Math]::Round($Value) }
    return [int][Math]::Round($Value * $script:uiScale)
}

function Write-Perf([string]$Label, [long]$Ms) {
    if (-not $env:DLSS_PERF) { return }
    try { [IO.File]::AppendAllText((Join-Path $env:TEMP 'opencode\perf.log'), ((Get-Date).ToString('HH:mm:ss.fff') + '  ' + $Label + '  ' + $Ms + ' ms' + "`r`n")) } catch { }
}

Import-Module (Join-Path $PSScriptRoot 'Dlss5.Core.psm1') -Force
[Windows.Forms.Application]::EnableVisualStyles()

$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$batch = Join-Path $project '_scratch/dlss5-aio/20261106-093536'
$script:settingsDir = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'WOW-335-NR'
$script:settingsFile = Join-Path $script:settingsDir 'control-center.json'

function Read-ConsoleSettings {
    $result = [pscustomobject]@{ Client = ''; Stage = ''; Recent = @() }
    try {
        if (-not [IO.File]::Exists($script:settingsFile)) { return $result }
        $data = [IO.File]::ReadAllText($script:settingsFile, [Text.Encoding]::UTF8) | ConvertFrom-Json
        if ($data.PSObject.Properties.Name -contains 'client') { $result.Client = [string]$data.client }
        if ($data.PSObject.Properties.Name -contains 'stage') { $result.Stage = [string]$data.stage }
        if ($data.PSObject.Properties.Name -contains 'recent') {
            $list = @()
            foreach ($item in @($data.recent)) {
                if ($item -and $item.PSObject.Properties.Name -contains 'path' -and $item.path) {
                    $list += [pscustomobject]@{ Path = [string]$item.path; Name = $(if ($item.PSObject.Properties.Name -contains 'name') { [string]$item.name } else { '' }) }
                }
            }
            $result.Recent = $list
        }
    } catch { }
    return $result
}

function Get-ClientDisplayName([string]$Path) {
    if (-not $Path) { return '未选择客户端' }
    $leaf = Split-Path $Path -Leaf
    if ($leaf -match '^_') {
        $parent = Split-Path (Split-Path $Path -Parent) -Leaf
        if ($parent) { return $parent }
    }
    return $leaf
}

function Save-ConsoleSettings {
    try {
        [IO.Directory]::CreateDirectory($script:settingsDir) | Out-Null
        $existing = Read-ConsoleSettings
        $recent = @()
        if ($script:clientPath) {
            $recent += [pscustomobject]@{ path = [string]$script:clientPath; name = (Get-ClientDisplayName $script:clientPath) }
        }
        foreach ($item in @($existing.Recent)) {
            if (-not $item.Path) { continue }
            if ($script:clientPath -and $item.Path.Equals($script:clientPath, [StringComparison]::OrdinalIgnoreCase)) { continue }
            if (-not [IO.Directory]::Exists($item.Path)) { continue }
            $recent += [pscustomobject]@{ path = [string]$item.Path; name = $(if ($item.Name) { $item.Name } else { Get-ClientDisplayName $item.Path }) }
            if ($recent.Count -ge 5) { break }
        }
        $payload = [ordered]@{ client = [string]$script:clientPath; stage = [string]$script:stagePath; recent = @($recent) } | ConvertTo-Json -Depth 4
        [IO.File]::WriteAllText($script:settingsFile, $payload, [Text.UTF8Encoding]::new($false))
    } catch { }
}

function Test-ClientRoot([string]$Path) {
    if (-not $Path) { return $false }
    try {
        $item = Get-Item -LiteralPath $Path -Force
        if (-not $item.PSIsContainer) { return $false }
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { return $false }
        if (-not [IO.File]::Exists((Join-Path $Path 'wow.exe'))) { return $false }
        $data = Join-Path $Path 'Data'
        return ([IO.File]::Exists((Join-Path $data 'common.MPQ')) -or [IO.File]::Exists((Join-Path $data 'common-2.MPQ')))
    } catch { return $false }
}

function Find-ClientRoot {
    $found = [Collections.Generic.List[string]]::new()
    $skip = @('Windows', 'Program Files', 'Program Files (x86)', 'ProgramData', 'Users',
        '$Recycle.Bin', 'System Volume Information', 'Recovery', 'PerfLogs', 'Windows.old', 'AppData', 'node_modules', '_scratch')
    foreach ($drive in [IO.DriveInfo]::GetDrives()) {
        try {
            if (-not $drive.IsReady -or $drive.DriveType -ne [IO.DriveType]::Fixed) { continue }
            $root = $drive.RootDirectory.FullName
            $queue = [Collections.Generic.Queue[string]]::new()
            $queue.Enqueue($root)
            $visited = 0
            while ($queue.Count -gt 0 -and $visited -lt 700) {
                $dir = $queue.Dequeue()
                $visited++
                if (Test-ClientRoot $dir) { $found.Add($dir); continue }
                try {
                    foreach ($child in [IO.Directory]::EnumerateDirectories($dir)) {
                        $name = [IO.Path]::GetFileName($child)
                        if ($skip -contains $name) { continue }
                        $relative = $child.Substring($root.Length).Trim('\', '/')
                        if (($relative -split '[\\/]').Count -gt 4) { continue }
                        try {
                            if ([IO.File]::GetAttributes($child) -band [IO.FileAttributes]::ReparsePoint) { continue }
                        } catch { continue }
                        $queue.Enqueue($child)
                    }
                } catch { }
            }
        } catch { }
    }
    if ($found.Count -eq 0) { return '' }
    return ($found | Sort-Object `
        @{ Expression = { if ([IO.File]::Exists((Join-Path $_ 'host64/dlss5-install-record.json'))) { 0 } else { 1 } } },
        @{ Expression = { if ([IO.File]::Exists((Join-Path $_ 'host64/ReShade.ini'))) { 0 } else { 1 } } },
        @{ Expression = { if ([IO.Directory]::Exists((Join-Path $_ 'Data/zhCN'))) { 0 } else { 1 } } },
        @{ Expression = { try { (Get-Item -LiteralPath (Join-Path $_ 'WTF/Config.wtf')).LastWriteTime } catch { [DateTime]::MinValue } }; Descending = $true },
        @{ Expression = { $_.Length } } | Select-Object -First 1)
}

$saved = Read-ConsoleSettings
if (-not $Client -and $saved.Client -and (Test-ClientRoot $saved.Client)) { $Client = $saved.Client }
if (-not $Client) {
    foreach ($probe in @($PSScriptRoot, (Split-Path $PSScriptRoot -Parent))) {
        if (Test-ClientRoot $probe) { $Client = $probe; break }
    }
}
if (-not $Client) { $Client = Find-ClientRoot }
if (-not $Stage) { $Stage = $saved.Stage }
if (-not $Stage) {
    $Stage = Join-Path $batch 'candidate/staged/control-center-candidate-20261106'
    if (-not [IO.Directory]::Exists($Stage)) {
        try {
            $latest = Get-ChildItem -LiteralPath (Join-Path $project '_scratch/dlss5-aio') -Directory -Recurse -Depth 4 -Filter 'control-center-candidate-*' -ErrorAction Stop |
                Sort-Object LastWriteTime -Descending | Select-Object -First 1
            if ($latest) { $Stage = $latest.FullName }
        } catch { }
    }
}
if (-not $Watcher) { $Watcher = Join-Path $PSScriptRoot 'bin/window_watch.exe' }

function C([string]$Hex) {
    return [Drawing.Color]::FromArgb(255,
        [Convert]::ToInt32($Hex.Substring(0, 2), 16),
        [Convert]::ToInt32($Hex.Substring(2, 2), 16),
        [Convert]::ToInt32($Hex.Substring(4, 2), 16))
}

$script:kBg = C '0F0C08'
$script:kChrome = C '14100A'
$script:kSurface = C '1B1610'
$script:kSurface2 = C '231C14'
$script:kSurface3 = C '2C2318'
$script:kLine = C '3A2E1F'
$script:kLineSoft = C '2A2117'
$script:kText = C 'EADFC6'
$script:kText2 = C 'C2B295'
$script:kText3 = C '9A8A6D'
$script:kAccent = C 'C8A45C'
$script:kAccentBright = C 'E3C884'
$script:kJade = C '6DAB86'
$script:kJadeText = C 'C9E7D6'
$script:kWarn = C 'D98A4A'
$script:kDanger = C 'BF5F52'
$script:kInk = C '1A1206'

# zh: 设计稿按 600×900 逻辑像素绘制，U() 与字号统一乘系统 DPI（125% → 750×1125），
# 只有一条缩放链，文字按物理像素渲染，避免二次拉伸导致的发虚。
$script:uiScale = [double][Wow335Ui.Dpi]::Scale
function New-UiFont([string]$Family, [double]$Pixels, [Drawing.FontStyle]$Style = [Drawing.FontStyle]::Regular) {
    return [Drawing.Font]::new($Family, [single]($Pixels * $script:uiScale), $Style, [Drawing.GraphicsUnit]::Pixel)
}
$script:fontBodyPx = 12.0
$script:fontSmallPx = 11.0
$script:fontTitlePx = 18.0
$script:fontHeroPx = 20.0
$script:fontGaugePx = 40.0
$script:fBody = New-UiFont 'Microsoft YaHei UI' $script:fontBodyPx
$script:fSmall = New-UiFont 'Microsoft YaHei UI' $script:fontSmallPx
$script:fBold = New-UiFont 'Microsoft YaHei UI' $script:fontBodyPx ([Drawing.FontStyle]::Bold)
$script:fTitle = New-UiFont 'Microsoft YaHei UI' $script:fontTitlePx ([Drawing.FontStyle]::Bold)
$script:fHero = New-UiFont 'Constantia' $script:fontHeroPx
$script:fMono = New-UiFont 'Consolas' $script:fontBodyPx
$script:fGauge = New-UiFont 'Constantia' $script:fontGaugePx
$script:fStat = New-UiFont 'Constantia' 15.0 ([Drawing.FontStyle]::Bold)
$script:fBrand = New-UiFont 'Constantia' 15.0 ([Drawing.FontStyle]::Bold)

function ConvertTo-FullPath([string]$Path) {
    if (-not $Path) { return '' }
    try { return [IO.Path]::GetFullPath($Path) } catch { return $Path }
}

$script:clientPath = ConvertTo-FullPath $Client
$script:stagePath = ConvertTo-FullPath $Stage
$script:watcherPath = ConvertTo-FullPath $Watcher
if ($script:clientPath -and (Test-ClientRoot $script:clientPath)) { Save-ConsoleSettings }
$script:view = 'main'
$script:wizStep = 1
$script:preview = $null
$script:status = $null
$script:checks = @()
$script:installResult = $null
$script:installError = $null
$script:lastPanelLog = $null
$script:lastWatchLog = $null
$script:logText = "请选择客户端目录，点击「检查状态」。`r`n安装只写入发行包清单内的文件。`r`n修改前会先备份到 host64\dlss5-backup。"
$script:logBox = $null
$script:mainStatus = $null
$script:mainLive = $null
$script:lastSample = $null
$script:fpsSource = $null
$script:fpsOutput = $null
$script:telemetryModule = $null
$script:showAdvanced = $false
$script:maintNote = ''
$script:unResult = $null
$script:wizBusy = $false
$script:gpuCache = $null
$script:gpuCacheAt = [DateTime]::MinValue
$script:pathTip = $null

$ContentWidth = (U 568)

function Set-Rounded($Control, [int]$Radius) {
    $path = [Drawing.Drawing2D.GraphicsPath]::new()
    $d = $Radius * 2
    $w = $Control.Width - 1
    $h = $Control.Height - 1
    $path.AddArc(0, 0, $d, $d, 180, 90)
    $path.AddArc($w - $d, 0, $d, $d, 270, 90)
    $path.AddArc($w - $d, $h - $d, $d, $d, 0, 90)
    $path.AddArc(0, $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    $Control.Region = [Drawing.Region]::new($path)
    $path.Dispose()
    $Control | Add-Member -NotePropertyName RoundedRadius -NotePropertyValue $Radius -Force
}

function New-Label([string]$Text, $Font = $null, $Color = $null, [int]$Width = 0) {
    $label = [Windows.Forms.Label]::new()
    $label.Text = $Text
    $label.AutoSize = $true
    $label.BackColor = [Drawing.Color]::Transparent
    $label.Font = if ($Font) { $Font } else { $script:fBody }
    $label.ForeColor = if ($null -ne $Color) { $Color } else { $script:kText2 }
    $label.Margin = [Windows.Forms.Padding]::new((U 0), (U 0), (U 0), (U 0))
    if ($Width -gt 0) { $label.MaximumSize = [Drawing.Size]::new($Width, (U 0)) }
    return $label
}

function New-Button([string]$Text, [string]$Kind = 'secondary', [int]$Width = 96, [int]$Height = 32) {
    $button = [Windows.Forms.Button]::new()
    $button.Text = $Text
    $button.Width = $Width
    $button.Height = $Height
    $button.FlatStyle = 'Flat'
    $button.UseVisualStyleBackColor = $false
    $button.Font = $script:fBody
    $button.Cursor = [Windows.Forms.Cursors]::Hand
    $button.Margin = [Windows.Forms.Padding]::new((U 0), (U 0), (U 8), (U 0))
    $button.FlatAppearance.BorderSize = 0
    switch ($Kind) {
        'primary' {
            $button.BackColor = $script:kAccent
            $button.ForeColor = $script:kInk
            $button.FlatAppearance.MouseOverBackColor = $script:kAccentBright
            $button.FlatAppearance.MouseDownBackColor = $script:kAccent
        }
        'danger' {
            $button.BackColor = $script:kDanger
            $button.ForeColor = [Drawing.Color]::White
            $button.FlatAppearance.MouseOverBackColor = C 'D07163'
            $button.FlatAppearance.MouseDownBackColor = $script:kDanger
        }
        'ghost' {
            $button.BackColor = $script:kSurface
            $button.ForeColor = $script:kText2
            $button.FlatAppearance.MouseOverBackColor = $script:kSurface2
            $button.FlatAppearance.MouseDownBackColor = $script:kSurface3
        }
        default {
            $button.BackColor = $script:kSurface2
            $button.ForeColor = $script:kText
            $button.FlatAppearance.BorderSize = 1
            $button.FlatAppearance.BorderColor = $script:kLine
            $button.FlatAppearance.MouseOverBackColor = $script:kSurface3
            $button.FlatAppearance.MouseDownBackColor = $script:kSurface3
        }
    }
    return $button
}

function Set-Buffered($Control) {
    # zh: 双缓冲减少切页/重绘闪烁；Control.DoubleBuffered 是受保护属性，用反射设置。
    try {
        $prop = $Control.GetType().GetProperty('DoubleBuffered', [Reflection.BindingFlags]'Instance,NonPublic')
        if ($null -ne $prop) { $prop.SetValue($Control, $true) }
    } catch { }
}

function New-Card([int]$Height, $Fill = $null, $Border = $null, [int]$Width = 0) {
    # zh: 卡片高度由调用方按内容算好（行数 × 行高）。不要给卡片开 AutoSize：
    # 绝对定位的透明子控件配合 AutoSize 会被包围盒算飞，行高翻倍后互相重叠
    # （20261113 实测截图，启动检查卡整块空白）。
    $cardWidth = if ($Width -gt 0) { $Width } else { $ContentWidth }
    $card = [Windows.Forms.Panel]::new()
    $card.Width = $cardWidth
    $card.Height = [Math]::Max($Height, 1)
    $card.BackColor = if ($Fill) { $Fill } else { $script:kSurface2 }
    $card.Margin = [Windows.Forms.Padding]::new((U 0), (U 0), (U 0), (U 12))
    Set-Buffered $card
    Set-Rounded $card (U 9)
    if ($Border) {
        $card.Tag = $Border
        $card.Add_Paint({
            param($sender, $eventArgs)
            $graphics = $eventArgs.Graphics
            $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
            $pen = [Drawing.Pen]::new([Drawing.Color]$sender.Tag, 1)
            $shape = [Drawing.Drawing2D.GraphicsPath]::new()
            $d = 18
            $w = $sender.Width - 2
            $h = $sender.Height - 2
            $shape.AddArc(1, 1, $d, $d, 180, 90)
            $shape.AddArc($w - $d, 1, $d, $d, 270, 90)
            $shape.AddArc($w - $d, $h - $d, $d, $d, 0, 90)
            $shape.AddArc(1, $h - $d, $d, $d, 90, 90)
            $shape.CloseFigure()
            $graphics.DrawPath($pen, $shape)
            $pen.Dispose()
            $shape.Dispose()
        })
    }
    return $card
}

function New-SectionTitle([string]$Text, [string]$Note = '') {
    $row = [Windows.Forms.Panel]::new()
    $row.Width = $ContentWidth
    $row.Height = (U 26)
    $row.BackColor = [Drawing.Color]::Transparent
    $row.Margin = [Windows.Forms.Padding]::new((U 0), (U 4), (U 0), (U 5))
    $mark = New-Label '◆' $script:fSmall $script:kAccent
    $mark.Location = [Drawing.Point]::new((U 0), (U 5))
    $row.Controls.Add($mark)
    $title = New-Label $Text $script:fBold $script:kText
    $title.Location = [Drawing.Point]::new((U 14), (U 2))
    $row.Controls.Add($title)
    $noteLabel = New-Label $Note $script:fSmall $script:kText3
    $noteLabel.Location = [Drawing.Point]::new((14 + $title.PreferredWidth + (U 8)), (U 4))
    $row.Controls.Add($noteLabel)
    $row | Add-Member -NotePropertyName SectionTitleLabel -NotePropertyValue $title
    $row | Add-Member -NotePropertyName SectionNoteLabel -NotePropertyValue $noteLabel
    $row.Add_Paint({
        param($sender, $eventArgs)
        $startX = $sender.SectionNoteLabel.Right + (U 10)
        if ($startX -gt ($sender.Width - (U 30))) { $startX = $sender.Width - (U 30) }
        $pen = [Drawing.Pen]::new($script:kLineSoft, 1)
        $y = [int]($sender.Height / 2) + (U 2)
        $eventArgs.Graphics.DrawLine($pen, $startX, $y, ($sender.Width - 2), $y)
        $pen.Dispose()
    })
    return $row
}

function Set-SectionTitle($Row, [string]$Text, [string]$Note) {
    if ($null -eq $Row) { return }
    $Row.SectionTitleLabel.Text = $Text
    $Row.SectionNoteLabel.Text = $Note
    $Row.SectionNoteLabel.Location = [Drawing.Point]::new((16 + $Row.SectionTitleLabel.PreferredWidth + (U 10)), (U 7))
    $Row.Invalidate()
}

function New-KvRow([string]$Key, [string]$Value, $ValueColor = $null, [int]$Height = 36) {
    $row = [Windows.Forms.Panel]::new()
    $row.Width = $ContentWidth
    $row.Height = $Height
    $row.BackColor = [Drawing.Color]::Transparent
    $row.Margin = [Windows.Forms.Padding]::new((U 0), (U 0), (U 0), (U 0))

    $keyLabel = New-Label $Key $script:fSmall $script:kText3
    $keyLabel.Location = [Drawing.Point]::new((U 16), (U 6))
    $keyLabel.Width = (U 54)
    $row.Controls.Add($keyLabel)
    $valueLabel = New-Label $Value $script:fBody $(if ($null -ne $ValueColor) { $ValueColor } else { $script:kText2 })
    $valueLabel.Location = [Drawing.Point]::new((U 76), (U 3))
    $valueLabel.MaximumSize = [Drawing.Size]::new(($ContentWidth - (U 96)), (U 0))
    $row.Controls.Add($valueLabel)
    return $row
}

function New-FileRow([string]$Name, [string]$Tag, $TagColor, [string]$Note = '') {
    $row = [Windows.Forms.Panel]::new()
    $row.Width = $ContentWidth
    $row.Height = (U 40)
    $row.BackColor = [Drawing.Color]::Transparent
    $row.Margin = [Windows.Forms.Padding]::new((U 0), (U 0), (U 0), (U 1))

    $nameLabel = New-Label $Name $script:fMono $script:kText
    $nameLabel.AutoSize = $false
    $nameLabel.AutoEllipsis = $true
    $nameLabel.TextAlign = 'MiddleLeft'
    $nameLabel.Width = ($ContentWidth - (U 116))
    $nameLabel.Height = [int]($script:fMono.GetHeight() + (U 2))
    $nameLabel.Location = [Drawing.Point]::new((U 16), (U 9))
    $row.Controls.Add($nameLabel)
    $tagLabel = New-Label $Tag $script:fSmall $TagColor
    $tagLabel.Location = [Drawing.Point]::new(($ContentWidth - (U 92)), 9)
    $tagLabel.Width = (U 76)
    $tagLabel.TextAlign = 'MiddleRight'
    $row.Controls.Add($tagLabel)
    if ($Note) {
        $noteLabel = New-Label $Note $script:fSmall $script:kText3
        $noteLabel.Location = [Drawing.Point]::new(16 + $nameLabel.PreferredWidth + 10, 9)
        $row.Controls.Add($noteLabel)
    }
    return $row
}

function New-CheckRow([bool]$Ok, [string]$Text, [string]$Detail = '', [string]$Status = '', $StatusColor = $null, [int]$Reserve = 0) {
    $row = [Windows.Forms.Panel]::new()
    $row.Width = $ContentWidth
    $row.Height = (U 44)
    $row.BackColor = [Drawing.Color]::Transparent
    $row.Margin = [Windows.Forms.Padding]::new((U 0), (U 0), (U 0), (U 1))

    $icon = New-Label $(if ($Ok) { '✓' } else { '!' }) $script:fBold $(if ($Ok) { $script:kJade } else { $script:kWarn })
    $icon.Location = [Drawing.Point]::new((U 16), (U 10))
    $row.Controls.Add($icon)
    # zh: 标题只占一行并右省略；换行交给下面的说明行。旧版两行都自动换行，
    # 标题换行后与固定 y 的说明文字叠在一起（20261113 实测）。
    $main = New-Label $Text $script:fBody $script:kText
    $main.AutoSize = $false
    $main.AutoEllipsis = $true
    $main.TextAlign = 'MiddleLeft'
    $main.Width = ($ContentWidth - (U 56) - $(if ($Status) { (U 84) } else { 0 }) - $Reserve)
    $main.Height = [int]($script:fBody.GetHeight() + (U 2))
    $main.Location = [Drawing.Point]::new((U 36), (U 5))
    $row.Controls.Add($main)
    if ($Detail) {
        $detailLabel = New-Label $Detail $script:fSmall $(if ($Ok) { $script:kText3 } else { $script:kWarn })
        $detailLabel.Location = [Drawing.Point]::new((U 36), ($main.Top + $main.Height + (U 1)))
        $detailLabel.MaximumSize = [Drawing.Size]::new(($ContentWidth - (U 56) - $(if ($Status) { (U 84) } else { 0 }) - $Reserve), (U 0))
        $row.Controls.Add($detailLabel)
    }
    if ($Status) {
        $statusLabel = New-Label $Status $script:fSmall $(if ($null -ne $StatusColor) { $StatusColor } else { $(if ($Ok) { $script:kJade } else { $script:kWarn }) })
        $statusLabel.AutoSize = $false
        $statusLabel.TextAlign = 'MiddleRight'
        $statusLabel.Size = [Drawing.Size]::new((U 76), (U 18))
        $statusLabel.Location = [Drawing.Point]::new(($ContentWidth - (U 92)), (U 13))
        $row.Controls.Add($statusLabel)
    }
    return $row
}

function Set-LogBoxText($Box, [string]$Text) {
    if ($null -eq $Box -or $Box.IsDisposed) { return }
    $Box.Text = $Text
    try {
        $Box.SelectionStart = $Box.TextLength
        $Box.ScrollToCaret()
    } catch { }
}

function Add-Log([string]$Text) {
    $stamp = Get-Date -Format 'HH:mm:ss'
    $script:logText = $script:logText + "`r`n[$stamp] " + $Text
    Set-LogBoxText $script:logBox $script:logText
}

if (-not ('Dlss5.ControlCenter.ScrollTheme' -as [type])) {
    Add-Type -Namespace Dlss5.ControlCenter -Name ScrollTheme -MemberDefinition @'
[DllImport("uxtheme.dll", CharSet = CharSet.Unicode)]
public static extern int SetWindowTheme(IntPtr hWnd, string subApp, string subIdList);
'@
}

function Set-DarkScrollHandle([IntPtr]$Handle) {
    try { [Dlss5.ControlCenter.ScrollTheme]::SetWindowTheme($Handle, 'DarkMode_Explorer', $null) | Out-Null } catch { }
}

function Enable-DarkScroll($Control) {
    $Control.Add_HandleCreated({ param($sender, $e) Set-DarkScrollHandle $sender.Handle })
    if ($Control.IsHandleCreated) { Set-DarkScrollHandle $Control.Handle }
}

function Get-ToneColors([string]$Tone) {
    switch ($Tone) {
        'ok' { return [pscustomobject]@{ Dot = $script:kJade; Text = $script:kJadeText } }
        'warn' { return [pscustomobject]@{ Dot = $script:kWarn; Text = $script:kWarn } }
        'accent' { return [pscustomobject]@{ Dot = $script:kAccent; Text = $script:kText } }
        default { return [pscustomobject]@{ Dot = $script:kText3; Text = $script:kText3 } }
    }
}

function New-DotChip([string]$Label, [string]$Value, [string]$Tone = 'muted') {
    $colors = Get-ToneColors $Tone
    $panel = [Windows.Forms.Panel]::new()
    $panel.Height = (U 26)
    $panel.BackColor = $script:kSurface
    $panel.Margin = [Windows.Forms.Padding]::new((U 0), (U 0), (U 7), (U 0))
    $panel | Add-Member -NotePropertyName DotColor -NotePropertyValue $colors.Dot
    $panel | Add-Member -NotePropertyName FillColor -NotePropertyValue $script:kSurface2
    $textLabel = $null
    $valueLabel = New-Label $Value $script:fSmall $colors.Text
    $valueLabel.AutoSize = $false
    $valueLabel.BackColor = $script:kSurface2
    $valueLabel.Height = (U 18)
    $valueLabel.Width = [Windows.Forms.TextRenderer]::MeasureText($Value, $script:fSmall).Width
    $panel.Controls.Add($valueLabel)
    if ($Label) {
        $textLabel = New-Label $Label $script:fSmall $script:kText2
        $textLabel.AutoSize = $false
        $textLabel.BackColor = $script:kSurface2
        $textLabel.Height = (U 18)
        $textLabel.Width = [Windows.Forms.TextRenderer]::MeasureText($Label, $script:fSmall).Width
        $textLabel.Location = [Drawing.Point]::new((U 20), (U 4))
        $panel.Controls.Add($textLabel)
        $valueLabel.Location = [Drawing.Point]::new(($textLabel.Right + (U 5)), (U 4))
    } else {
        $valueLabel.Location = [Drawing.Point]::new((U 20), (U 4))
    }
    $panel.Width = $valueLabel.Right + (U 10)
    $panel.Add_Paint({
        param($sender, $eventArgs)
        $g = $eventArgs.Graphics
        $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $w = [single]($sender.Width - 1)
        $h = [single]($sender.Height - 1)
        $path = [Drawing.Drawing2D.GraphicsPath]::new()
        $path.AddArc(0, 0, $h, $h, 90, 180)
        $path.AddArc(($w - $h), 0, $h, $h, 270, 180)
        $path.CloseFigure()
        $fill = [Drawing.SolidBrush]::new([Drawing.Color]$sender.FillColor)
        $g.FillPath($fill, $path)
        $fill.Dispose()
        $pen = [Drawing.Pen]::new($script:kLine, 1)
        $g.DrawPath($pen, $path)
        $pen.Dispose()
        $path.Dispose()
        $brush = [Drawing.SolidBrush]::new([Drawing.Color]$sender.DotColor)
        $r = (U 3)
        $g.FillEllipse($brush, (U 12) - $r, [int]($sender.Height / 2) - $r, $r * 2, $r * 2)
        $brush.Dispose()
    })
    return [pscustomobject]@{ Panel = $panel; Text = $textLabel; Value = $valueLabel; Tone = $Tone }
}

function Set-DotChip($Chip, [string]$Value, [string]$Tone, [string]$Label = $null) {
    if ($null -eq $Chip) { return }
    $colors = Get-ToneColors $Tone
    $Chip.Tone = $Tone
    if (-not [string]::IsNullOrEmpty($Label) -and $null -ne $Chip.Text) { $Chip.Text.Text = $Label }
    $Chip.Value.Text = $Value
    $Chip.Value.Width = [Windows.Forms.TextRenderer]::MeasureText($Value, $script:fSmall).Width
    $Chip.Value.ForeColor = $colors.Text
    if ($null -ne $Chip.Text) {
        $Chip.Text.Width = [Windows.Forms.TextRenderer]::MeasureText($Chip.Text.Text, $script:fSmall).Width
        $Chip.Value.Location = [Drawing.Point]::new(($Chip.Text.Right + (U 5)), (U 4))
    } else {
        $Chip.Value.Location = [Drawing.Point]::new((U 20), (U 4))
    }
    $Chip.Panel.DotColor = $colors.Dot
    $Chip.Panel.Width = $Chip.Value.Right + (U 10)
    $Chip.Panel.Invalidate()
}

function New-BrandMark([int]$Size) {
    $panel = [Windows.Forms.Panel]::new()
    $panel.Width = $Size
    $panel.Height = $Size
    $panel.BackColor = [Drawing.Color]::Transparent
    $panel.Add_Paint({
        param($sender, $eventArgs)
        $eventArgs.Graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $g = $eventArgs.Graphics
        $cx = $sender.Width / 2.0
        $cy = $sender.Height / 2.0
        $r = [Math]::Min($sender.Width, $sender.Height) / 2.0 - 0.5
        $outer = [Drawing.PointF[]]@(
            [Drawing.PointF]::new([single]$cx, [single]($cy - $r)),
            [Drawing.PointF]::new([single]($cx + $r), [single]$cy),
            [Drawing.PointF]::new([single]$cx, [single]($cy + $r)),
            [Drawing.PointF]::new([single]($cx - $r), [single]$cy))
        $brush = [Drawing.SolidBrush]::new($script:kAccentBright)
        $g.FillPolygon($brush, $outer)
        $brush.Dispose()
        $inner = [Drawing.PointF[]]@(
            [Drawing.PointF]::new([single]$cx, [single]($cy - $r + 4.2)),
            [Drawing.PointF]::new([single]($cx + $r - 4.2), [single]$cy),
            [Drawing.PointF]::new([single]$cx, [single]($cy + $r - 4.2)),
            [Drawing.PointF]::new([single]($cx - $r + 4.2), [single]$cy))
        $brush = [Drawing.SolidBrush]::new($script:kChrome)
        $g.FillPolygon($brush, $inner)
        $brush.Dispose()
        $pen = [Drawing.Pen]::new($script:kAccent, 1)
        $g.DrawPolygon($pen, $outer)
        $pen.Dispose()
    })
    return $panel
}

function New-StatTable {
    $wrap = [Windows.Forms.Panel]::new()
    $wrap.Width = $ContentWidth
    $wrap.Height = (U 150)
    $wrap.BackColor = $script:kLineSoft
    $wrap.Margin = [Windows.Forms.Padding]::new((U 0), (U 0), (U 0), (U 0))
    Set-Rounded $wrap (U 9)
    $rows = @(
        @{ Label = '分辨率'; Keys = @('resIn', 'resOut', 'scale'); Names = @('输入', '输出', '缩放'); Primary = $false },
        @{ Label = '性能'; Keys = @('latency', 'vram', 'gpu'); Names = @('延迟', '显存', '显卡'); Primary = $true },
        @{ Label = '计数'; Keys = @('nrFrames', 'srFrames', 'fgFrames'); Names = @('增强帧', '超分帧', '生成帧'); Primary = $false }
    )
    $cells = @{}
    $labelWidth = (U 44)
    $rowHeight = (U 48)
    $cellWidth = [int](($ContentWidth - $labelWidth - (U 7)) / 3)
    for ($r = 0; $r -lt $rows.Count; $r++) {
        $rowDef = $rows[$r]
        $rowPanel = [Windows.Forms.Panel]::new()
        $rowPanel.Location = [Drawing.Point]::new((U 1), ((U 1) + $r * ($rowHeight + (U 1))))
        $rowPanel.Size = [Drawing.Size]::new(($ContentWidth - (U 2)), $rowHeight)
        $rowPanel.BackColor = $script:kSurface
        $labelPanel = [Windows.Forms.Panel]::new()
        $labelPanel.Location = [Drawing.Point]::new(0, 0)
        $labelPanel.Size = [Drawing.Size]::new($labelWidth, $rowHeight)
        $labelPanel.BackColor = if ($rowDef.Primary) { C '2E2518' } else { $script:kSurface2 }
        $labelText = New-Label $rowDef.Label $script:fSmall $(if ($rowDef.Primary) { $script:kAccent } else { $script:kText3 })
        $labelText.AutoSize = $false
        $labelText.TextAlign = 'MiddleCenter'
        $labelText.Size = [Drawing.Size]::new($labelWidth, $rowHeight)
        $labelText.Location = [Drawing.Point]::new(0, 0)
        $labelPanel.Controls.Add($labelText)
        $rowPanel.Controls.Add($labelPanel)
        for ($c = 0; $c -lt 3; $c++) {
            $cell = [Windows.Forms.Panel]::new()
            $cell.Location = [Drawing.Point]::new(($labelWidth + (U 1) + $c * ($cellWidth + (U 1))), 0)
            $cell.Size = [Drawing.Size]::new($cellWidth, $rowHeight)
            $cell.BackColor = if ($rowDef.Primary) { $script:kSurface3 } else { $script:kSurface }
            $nameLabel = New-Label $rowDef.Names[$c] $script:fSmall $(if ($rowDef.Primary) { $script:kText2 } else { $script:kText3 })
            $nameLabel.Location = [Drawing.Point]::new((U 12), (U 9))
            $cell.Controls.Add($nameLabel)
            $valueLabel = New-Label '—' $script:fStat $script:kText
            $valueLabel.AutoSize = $false
            $valueLabel.AutoEllipsis = $true
            $valueLabel.TextAlign = 'MiddleLeft'
            $valueLabel.Location = [Drawing.Point]::new((U 11), (U 27))
            $valueLabel.Size = [Drawing.Size]::new(($cellWidth - (U 20)), (U 21))
            $cell.Controls.Add($valueLabel)
            $cells[$rowDef.Keys[$c]] = $valueLabel
            $rowPanel.Controls.Add($cell)
        }
        $wrap.Controls.Add($rowPanel)
    }
    return [pscustomobject]@{ Panel = $wrap; Cells = $cells }
}

function Set-StatTable($Table, $Values) {
    if ($null -eq $Table) { return }
    foreach ($key in $Values.Keys) {
        if ($Table.Cells.ContainsKey($key)) { $Table.Cells[$key].Text = [string]$Values[$key] }
    }
}

function New-Pipeline {
    $card = New-Card (U 34) $script:kSurface2 $script:kLineSoft
    $card.Margin = [Windows.Forms.Padding]::new((U 0), (U 8), (U 0), (U 0))
    $flow = [Windows.Forms.FlowLayoutPanel]::new()
    $flow.Location = [Drawing.Point]::new((U 12), (U 6))
    $flow.AutoSize = $false
    $flow.Width = ($ContentWidth - (U 24))
    $flow.Height = (U 22)
    $flow.WrapContents = $false
    $flow.BackColor = [Drawing.Color]::Transparent
    $steps = @{}
    $defs = @(@('capture', '抓帧'), @('nr', '画面增强'), @('sr', '超分 / 抗锯齿'), @('fg', '补帧'))
    for ($i = 0; $i -lt $defs.Count; $i++) {
        $dot = New-Label '●' $script:fSmall $script:kText3
        $dot.Margin = [Windows.Forms.Padding]::new((U 0), (U 0), (U 4), (U 0))
        $text = New-Label $defs[$i][1] $script:fSmall $script:kText3
        $text.Margin = [Windows.Forms.Padding]::new((U 0), (U 0), (U 0), (U 0))
        $flow.Controls.Add($dot)
        $flow.Controls.Add($text)
        $steps[$defs[$i][0]] = [pscustomobject]@{ Dot = $dot; Text = $text }
        if ($i -lt $defs.Count - 1) {
            $arrow = New-Label '→' $script:fSmall (C '5A4A34')
            $arrow.Margin = [Windows.Forms.Padding]::new((U 8), (U 0), (U 8), (U 0))
            $flow.Controls.Add($arrow)
        }
    }
    $card.Controls.Add($flow)
    return [pscustomobject]@{ Panel = $card; Steps = $steps }
}

function Set-PipelineState($Pipeline, [bool]$Capture, [bool]$Nr, [bool]$Sr, [bool]$Fg) {
    if ($null -eq $Pipeline) { return }
    $map = @{ capture = $Capture; nr = $Nr; sr = $Sr; fg = $Fg }
    foreach ($key in $map.Keys) {
        $step = $Pipeline.Steps[$key]
        $on = [bool]$map[$key]
        $step.Dot.ForeColor = if ($on) { $script:kJade } else { $script:kText3 }
        $step.Text.ForeColor = if ($on) { $script:kText2 } else { $script:kText3 }
    }
}

function New-ConsoleEmpty {
    $card = New-Card (U 122) $script:kSurface2 $script:kLineSoft
    $icon = [Windows.Forms.Panel]::new()
    $icon.Location = [Drawing.Point]::new((U 18), (U 18))
    $icon.Size = [Drawing.Size]::new((U 34), (U 34))
    $icon.BackColor = C '2C2318'
    $icon.Tag = C '5A4A2E'
    $icon.Add_Paint({
        param($sender, $eventArgs)
        $eventArgs.Graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $pen = [Drawing.Pen]::new([Drawing.Color]$sender.Tag, 1)
        $shape = [Drawing.Drawing2D.GraphicsPath]::new()
        $d = 12
        $shape.AddArc(0, 0, $d, $d, 180, 90)
        $shape.AddArc(($sender.Width - $d - 1), 0, $d, $d, 270, 90)
        $shape.AddArc(($sender.Width - $d - 1), ($sender.Height - $d - 1), $d, $d, 0, 90)
        $shape.AddArc(0, ($sender.Height - $d - 1), $d, $d, 90, 90)
        $shape.CloseFigure()
        $eventArgs.Graphics.DrawPath($pen, $shape)
        $pen.Dispose()
        $shape.Dispose()
    })
    Set-Rounded $icon (U 9)
    $glyph = New-Label '◉' $script:fBold $script:kAccent
    $glyph.Location = [Drawing.Point]::new((U 9), (U 6))
    $icon.Controls.Add($glyph)
    $card.Controls.Add($icon)
    $title = New-Label '等待游戏启动' $script:fBold $script:kText
    $title.Location = [Drawing.Point]::new((U 66), (U 18))
    $card.Controls.Add($title)
    $desc = New-Label '启动后将在这里显示分辨率、延迟与帧数。' $script:fSmall $script:kText3
    $desc.Location = [Drawing.Point]::new((U 66), (U 40))
    $card.Controls.Add($desc)
    $divider = [Windows.Forms.Panel]::new()
    $divider.Location = [Drawing.Point]::new((U 18), (U 78))
    $divider.Size = [Drawing.Size]::new(($ContentWidth - (U 36)), 1)
    $divider.BackColor = $script:kLineSoft
    $card.Controls.Add($divider)
    $ready = [Windows.Forms.FlowLayoutPanel]::new()
    $ready.Location = [Drawing.Point]::new((U 18), (U 88))
    $ready.Size = [Drawing.Size]::new(($ContentWidth - (U 36)), (U 22))
    $ready.WrapContents = $false
    $ready.BackColor = [Drawing.Color]::Transparent
    $clientReady = New-Label '✓ 客户端已就绪' $script:fSmall $script:kText2
    $clientReady.Margin = [Windows.Forms.Padding]::new((U 0), (U 0), (U 18), (U 0))
    $gpuReady = New-Label '✓ 显卡已识别' $script:fSmall $script:kText2
    $gpuReady.Margin = [Windows.Forms.Padding]::new((U 0), (U 0), (U 18), (U 0))
    $profileReady = New-Label '● 档位 均衡 · 模型 自然' $script:fSmall $script:kText2
    $ready.Controls.Add($clientReady)
    $ready.Controls.Add($gpuReady)
    $ready.Controls.Add($profileReady)
    $card.Controls.Add($ready)
    return [pscustomobject]@{ Panel = $card; Client = $clientReady; Gpu = $gpuReady; Profile = $profileReady }
}

function Get-TierInfo {
    $model = ''
    $values = $null
    try { $values = Get-Dlss5AdvancedSettings -Client $script:clientPath } catch { }
    if ($null -ne $values) { $model = [string]$values.Model }
    $modelName = switch ($model) { '1' { '默认' } '2' { '自然' } '3' { '电影' } default { '默认' } }
    $tierName = '自定义'
    if ($null -ne $values) {
        $nr = ([string]$values.NeuralRendering -eq '1')
        $tiers = @(
            @{ Name = '原画'; Nr = $false; Intensity = 1.0; Tone = 1.0; Structure = 1.0 },
            @{ Name = '轻量'; Nr = $true; Intensity = 0.20; Tone = 1.0; Structure = 0.10 },
            @{ Name = '均衡'; Nr = $true; Intensity = 0.35; Tone = 1.0; Structure = 0.25 },
            @{ Name = '浓郁'; Nr = $true; Intensity = 0.60; Tone = 1.30; Structure = 0.50 }
        )
        $parse = { param($text) $out = 0.0; if ([double]::TryParse([string]$text, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$out)) { return $out }; return $null }
        $intensity = & $parse $values.Intensity
        $tone = & $parse $values.LocalTone
        $structure = & $parse $values.LocalStructure
        foreach ($tier in $tiers) {
            if ($null -eq $intensity -or $null -eq $tone -or $null -eq $structure) { continue }
            if ([bool]$tier.Nr -ne $nr) { continue }
            if ([Math]::Abs($intensity - [double]$tier.Intensity) -le 0.02 -and
                [Math]::Abs($tone - [double]$tier.Tone) -le 0.02 -and
                [Math]::Abs($structure - [double]$tier.Structure) -le 0.02) { $tierName = $tier.Name; break }
        }
    }
    return [pscustomobject]@{ Tier = $tierName; Model = $modelName }
}

function Get-GpuStats {
    $now = Get-Date
    if ($null -ne $script:gpuCache -and ($now - $script:gpuCacheAt).TotalSeconds -lt 5) { return $script:gpuCache }
    $script:gpuCacheAt = $now
    $script:gpuCache = $null
    try {
        $memory = @(Get-CimInstance -ClassName Win32_PerfFormattedData_GPUPerformanceCounters_GPUAdapterMemory -ErrorAction Stop)
        $vram = ($memory | Measure-Object -Property DedicatedUsage -Sum).Sum
        $engines = @(Get-CimInstance -ClassName Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine -ErrorAction Stop |
            Where-Object { [string]$_.Name -like '*engtype_3D*' })
        $load = ($engines | Measure-Object -Property UtilizationPercentage -Sum).Sum
        $result = [pscustomobject]@{ Vram = $(if ($vram -gt 0) { $vram / 1GB } else { $null }); Load = $(if ($load -gt 0 -and $load -lt 100) { $load } else { $null }) }
        $script:gpuCache = $result
        return $result
    } catch { }
    return $null
}

function New-LogBox([int]$Height) {
    $card = New-Card $Height $script:kSurface2 $script:kLineSoft
    $box = [Windows.Forms.TextBox]::new()
    $box.Multiline = $true
    $box.ReadOnly = $true
    $box.ScrollBars = 'Vertical'
    $box.WordWrap = $true
    $box.BorderStyle = 'None'
    $box.BackColor = $script:kSurface2
    $box.ForeColor = $script:kText2
    $box.Font = $script:fMono
    $box.Location = [Drawing.Point]::new((U 12), (U 10))
    $box.Width = ($ContentWidth - (U 25))
    $box.Height = ($Height - (U 20))
    $card.Controls.Add($box)
    # zh: 旧版用一块覆盖片盖住系统滚动条；DPI 放大后覆盖片位置是逻辑值、
    # 反而挡住正文（20261113 实测截图）。改为只套深色滚动条主题。
    Enable-DarkScroll $box
    return [pscustomobject]@{ Panel = $card; Box = $box }
}

function Test-GameRunning {
    try {
        if (-not $script:clientPath) { return $false }
        $exe = Join-Path $script:clientPath 'wow.exe'
        if (-not [IO.File]::Exists($exe)) { return $false }
        $full = [IO.Path]::GetFullPath($exe)
        foreach ($process in @(Get-Process -Name 'wow' -ErrorAction SilentlyContinue)) {
            try {
                if ($process.Path -and $process.Path.Equals($full, [StringComparison]::OrdinalIgnoreCase)) { return $true }
            } catch { }
        }
    } catch { }
    return $false
}

function Start-PanelProcess {
    $panelExe = Join-Path $PSScriptRoot 'bin/dlss5-panel.exe'
    if (-not [IO.File]::Exists($panelExe)) { throw '面板程序缺失：bin/dlss5-panel.exe' }
    if (@(Get-Process -Name 'dlss5-panel' -ErrorAction SilentlyContinue).Count -gt 0) { throw '游戏内面板已在运行。' }
    $game = Join-Path $script:clientPath 'wow.exe'
    if (-not [IO.File]::Exists($game)) { throw '请先选择含 wow.exe 的客户端目录。' }
    $logDir = Join-Path $project ('_scratch/dlss5-aio/panel-logs/' + [guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($logDir) | Out-Null
    Start-Process -FilePath $panelExe -ArgumentList @('--game', ('"' + $game + '"')) -WindowStyle Hidden -RedirectStandardOutput (Join-Path $logDir 'stdout.log') -RedirectStandardError (Join-Path $logDir 'stderr.log')
    $script:lastPanelLog = $logDir
    return $logDir
}

function Get-AioHostId {
    try {
        $procs = @(Get-Process -Name 'AIO DLSS5 32-bit Wrapper' -ErrorAction SilentlyContinue)
        if ($procs.Count -eq 1) { return [int]$procs[0].Id }
    } catch { }
    return 0
}

function Read-TelemetrySample([int]$HostId) {
    if ($HostId -le 0) { return $null }
    if ($null -eq $script:telemetryModule) { $script:telemetryModule = Get-Module -Name 'Dlss5.Core' }
    if ($null -eq $script:telemetryModule) { return $null }
    try { return (& $script:telemetryModule { param($id) Read-AioTelemetry $id } $HostId) } catch { return $null }
}

function Get-ConfigWtfPath { return (Join-Path $script:clientPath 'WTF/Config.wtf') }

function Read-ConfigWtf([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        return [pscustomobject]@{ Text = [Text.Encoding]::UTF8.GetString($bytes, 3, $bytes.Length - 3); Bom = $true }
    }
    return [pscustomobject]@{ Text = [Text.Encoding]::Default.GetString($bytes); Bom = $false }
}

function Write-ConfigWtf([string]$Path, [string]$Text, [bool]$Bom) {
    $encoding = [Text.UTF8Encoding]::new($Bom)
    $temp = $Path + '.nr-tmp'
    try {
        [IO.File]::WriteAllText($temp, $Text, $encoding)
        [IO.File]::Replace($temp, $Path, [NullString]::Value)
    } finally {
        if ([IO.File]::Exists($temp)) { [IO.File]::Delete($temp) }
    }
}

function Read-FpsCap {
    $result = [pscustomobject]@{ Exists = $false; Value = $null; Back = $null }
    if (-not $script:clientPath) { return $result }
    $path = Get-ConfigWtfPath
    if (-not [IO.File]::Exists($path)) { return $result }
    $result.Exists = $true
    try {
        $text = (Read-ConfigWtf $path).Text
        $match = [regex]::Match($text, '(?im)^[ \t]*SET[ \t]+maxFPS[ \t]+"?(\d+)"?[ \t]*\r?$')
        if ($match.Success) { $result.Value = [int]$match.Groups[1].Value }
        $backMatch = [regex]::Match($text, '(?im)^[ \t]*SET[ \t]+maxFPSBK[ \t]+"?(\d+)"?[ \t]*\r?$')
        if ($backMatch.Success) { $result.Back = [int]$backMatch.Groups[1].Value }
    } catch { }
    return $result
}

function Save-FpsCap([int]$Front, [int]$Back) {
    foreach ($value in @($Front, $Back)) {
        if ($value -lt 0 -or $value -gt 999) { throw '帧率上限需在 0 到 999 之间。' }
    }
    $path = Get-ConfigWtfPath
    if (-not [IO.File]::Exists($path)) { throw '客户端内没有 WTF\Config.wtf，请先启动一次游戏。' }
    $original = Read-ConfigWtf $path
    $backup = $path + '.nr-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.bak'
    [IO.File]::Copy($path, $backup, $false)
    $text = $original.Text
    $pairs = @(@('maxFPS', $Front), @('maxFPSBK', $Back))
    foreach ($pair in $pairs) {
        $name = $pair[0]
        $value = $pair[1]
        $pattern = '(?im)^([ \t]*SET[ \t]+' + $name + '[ \t]+"?)\d+("?)(?=[ \t]*\r?$)'
        if ([regex]::IsMatch($text, $pattern)) {
            $text = [regex]::Replace($text, $pattern, ('${1}' + $value + '${2}'))
        } else {
            if ($text.Length -gt 0 -and -not $text.EndsWith("`n")) { $text += "`r`n" }
            $text += 'SET ' + $name + ' "' + $value + '"' + "`r`n"
        }
    }
    Write-ConfigWtf $path $text $original.Bom
    $check = Read-FpsCap
    if ($null -eq $check.Value -or $check.Value -ne $Front) { throw 'Config.wtf 写入后校验失败，已保留备份。' }
    if ($null -eq $check.Back -or $check.Back -ne $Back) { throw 'Config.wtf 后台帧率写入后校验失败，已保留备份。' }
    return $backup
}

$form = [Windows.Forms.Form]::new()
$form.Text = 'WOW-335-NR 控制台'
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'None'
$form.AutoScaleMode = 'None'
$form.ClientSize = [Drawing.Size]::new((U 600), (U 900))
$form.BackColor = $script:kSurface
$form.Font = $script:fBody
$form.KeyPreview = $true
Set-Buffered $form

$titleBar = [Windows.Forms.Panel]::new()
$titleBar.Dock = 'Top'
$titleBar.Height = (U 32)
$titleBar.BackColor = $script:kChrome
$titleBar.Add_Paint({
    param($sender, $eventArgs)
    $pen = [Drawing.Pen]::new($script:kLineSoft, 1)
    $eventArgs.Graphics.DrawLine($pen, 0, ($sender.Height - 1), $sender.Width, ($sender.Height - 1))
    $pen.Dispose()
})
$brand = New-BrandMark (U 14)
$brand.Location = [Drawing.Point]::new((U 16), (U 9))
$titleBar.Controls.Add($brand)
$titleText = New-Label 'WOW-335-NR' $script:fBrand $script:kText
$titleText.Location = [Drawing.Point]::new((U 36), (U 4))
$titleBar.Controls.Add($titleText)
$titleSub = New-Label '控制台' $script:fSmall $script:kText3
$titleSub.Location = [Drawing.Point]::new(($titleText.PreferredWidth + (U 44)), (U 9))
$titleBar.Controls.Add($titleSub)
$titleMeta = New-Label 'Mcode · v1.2.0' $script:fSmall $script:kText3
$titleMeta.Location = [Drawing.Point]::new(($form.ClientSize.Width - (U 112) - $titleMeta.PreferredWidth - (U 14)), (U 9))
$titleBar.Controls.Add($titleMeta)
$winButtons = [Windows.Forms.FlowLayoutPanel]::new()
$winButtons.Dock = 'Right'
$winButtons.Width = (U 98)
$winButtons.Height = (U 32)
$winButtons.BackColor = [Drawing.Color]::Transparent
$winButtons.FlowDirection = 'LeftToRight'
$winButtons.WrapContents = $false
$winButtons.Padding = [Windows.Forms.Padding]::new((U 0), (U 3), (U 8), (U 0))
$minimize = New-Button '—' 'ghost' (U 30) (U 24)
$maximize = New-Button '❐' 'ghost' (U 30) (U 24)
$close = New-Button '✕' 'ghost' (U 30) (U 24)
foreach ($wb in @($minimize, $maximize, $close)) {
    $wb.Margin = [Windows.Forms.Padding]::new(0)
    $wb.Padding = [Windows.Forms.Padding]::new(0)
    $wb.TabStop = $false
    $wb.BackColor = $script:kChrome
    $wb.FlatAppearance.BorderSize = 0
    $wb.FlatAppearance.MouseOverBackColor = $script:kSurface2
    $wb.FlatAppearance.MouseDownBackColor = $script:kSurface3
}
$minimize.Font = New-UiFont 'Microsoft YaHei UI' 13
$maximize.Font = New-UiFont 'Microsoft YaHei UI' 11
$close.Font = New-UiFont 'Microsoft YaHei UI' 13
$winButtons.Controls.Add($minimize)
$winButtons.Controls.Add($maximize)
$winButtons.Controls.Add($close)
$titleBar.Controls.Add($winButtons)
$minimize.Add_Click({ $form.WindowState = 'Minimized' })
$maximize.Add_Click({ if ($form.WindowState -eq 'Maximized') { $form.WindowState = 'Normal' } else { $form.WindowState = 'Maximized' } })
$close.Add_Click({ $form.Close() })
$form.Controls.Add($titleBar)
# zh: 无边框窗口拖动交给系统移动循环（WM_NCLBUTTONDOWN + HTCAPTION），
# 不再用 NCHITTEST 钩子或手算光标增量——旧方案在控件重挂/DPI 缩放下失效，
# 20261113 用户反馈窗口无法移动。
function Enable-CaptionDrag($Control) {
    $Control.Add_MouseDown({
        param($sender, $eventArgs)
        if ($eventArgs.Button -ne [Windows.Forms.MouseButtons]::Left) { return }
        try { [Wow335Ui.DragNative]::StartMove($form.Handle) } catch { }
    })
    $Control.Add_DoubleClick({
        param($sender, $eventArgs)
        if ($form.WindowState -eq 'Maximized') { $form.WindowState = 'Normal' } else { $form.WindowState = 'Maximized' }
    })
}
foreach ($dragTarget in @($titleBar, $titleText, $titleSub, $titleMeta, $brand)) { Enable-CaptionDrag $dragTarget }

function New-ViewShell([string]$Title, [string]$Subtitle, [string]$BackText, $BackAction) {
    $panel = [Windows.Forms.Panel]::new()
    $panel.Dock = 'Fill'
    $panel.BackColor = $script:kSurface
    $panel.Visible = $false

    $body = [Windows.Forms.FlowLayoutPanel]::new()
    $body.Dock = 'Fill'
    $body.FlowDirection = 'TopDown'
    $body.WrapContents = $false
    $body.AutoScroll = $true
    $body.BackColor = $script:kSurface
    $body.Padding = [Windows.Forms.Padding]::new((U 16), (U 14), (U 16), (U 12))

    $header = [Windows.Forms.Panel]::new()
    $header.Dock = 'Top'
    $header.Height = (U 80)
    $header.BackColor = $script:kSurface

    $back = New-Button ('← ' + $BackText) 'ghost' (U 112) (U 24)
    $back.Font = $script:fSmall
    $back.BackColor = $script:kSurface
    $back.Location = [Drawing.Point]::new((U 18), (U 8))
    $back.Visible = $false

    $headingLabel = New-Label $Title $script:fTitle $script:kText
    $headingLabel.Location = [Drawing.Point]::new((U 16), (U 12))
    $header.Controls.Add($headingLabel)

    # zh: 副标题按 540 宽换行后高度会变成两到三行，旧版固定 80 高，
    # 第二行会被步骤条/正文压住（20261113 用户反馈文字截断）。改为按实测高度撑开头部。
    $subWidth = (U 540)
    $subSize = [Windows.Forms.TextRenderer]::MeasureText($Subtitle, $script:fSmall, [Drawing.Size]::new($subWidth, 0), [Windows.Forms.TextFormatFlags]::WordBreak)
    $sub = New-Label $Subtitle $script:fSmall $script:kText3
    $sub.AutoSize = $false
    $sub.TextAlign = 'TopLeft'
    $sub.Location = [Drawing.Point]::new((U 16), (U 38))
    $sub.Size = [Drawing.Size]::new($subWidth, [int]$subSize.Height)
    $header.Controls.Add($sub)

    $extra = [Windows.Forms.Panel]::new()
    $extra.Location = [Drawing.Point]::new((U 16), ($sub.Top + $sub.Height + (U 10)))
    $extra.Width = (U 540)
    $extra.Height = (U 18)
    $extra.BackColor = [Drawing.Color]::Transparent
    $header.Controls.Add($extra)
    $header.Height = ($extra.Top + (U 26))

    $footer = [Windows.Forms.Panel]::new()
    $footer.Dock = 'Bottom'
    $footer.Height = (U 52)
    $footer.BackColor = $script:kSurface
    $footer.Tag = $script:kLineSoft
    $footer.Add_Paint({
        param($sender, $eventArgs)
        $pen = [Drawing.Pen]::new([Drawing.Color]$sender.Tag, 1)
        $eventArgs.Graphics.DrawLine($pen, 26, 0, $sender.Width - 26, 0)
        $pen.Dispose()
    })

    $panel.Controls.Add($body)
    $panel.Controls.Add($header)
    $panel.Controls.Add($footer)
    Set-Buffered $panel
    Set-Buffered $body
    Set-Buffered $header
    Set-Buffered $footer
    $form.Controls.Add($panel)
    return [pscustomobject]@{ Panel = $panel; Body = $body; Header = $header; Extra = $extra; Footer = $footer; Back = $back }
}

$mainView = New-ViewShell '控制台' '自动识别客户端；安装、维护并查看实时运行状态。' '退出' { $form.Close() }
$setupView = New-ViewShell '安装到客户端' '只向客户端写入发行包内的文件。已有的一致文件会跳过；需要改动的文件先备份，再合并写入本工具需要的配置项，不覆盖玩家原有设置。' '返回' { Show-View 'main' }
$maintView = New-ViewShell '维护' '查看已安装组件与备份位置，或移除本工具写入客户端的文件。' '返回' { Show-View 'main' }
$uninstallView = New-ViewShell '卸载组件' '逐项核对将在客户端中处理的文件，确认后执行。' '返回' { Show-View 'maint' }
$mainView.Back.Visible = $false
$mainView.Header.Visible = $false
$views = @{ main = $mainView; setup = $setupView; maint = $maintView; uninstall = $uninstallView }
# zh: 页面构件缓存。切页时若内容未变直接复用已建好的控件，避免每次整页重建 + 全窗重排
# （实测每次 0.7-2 秒卡顿）。数据变更方负责把对应页标脏。
$script:viewBuilt = @{ main = $false; setup = $false; maint = $false; uninstall = $false }
$script:viewDirty = @{ main = $true; setup = $true; maint = $true; uninstall = $true }
foreach ($shell in $views.Values) { Enable-DarkScroll $shell.Body }
$form.Controls.Add($titleBar)

function Clear-Panel($Panel) {
    $Panel.SuspendLayout()
    while ($Panel.Controls.Count -gt 0) {
        $child = $Panel.Controls[0]
        $Panel.Controls.RemoveAt(0)
        $child.Dispose()
    }
    $Panel.ResumeLayout()
}

function Set-Footer($View, [string]$LeftText, [scriptblock]$LeftAction, $Actions) {
    Clear-Panel $View.Footer
    if ($LeftText) {
        $left = New-Button $LeftText 'ghost' (U 110) (U 30)
        $left.BackColor = $script:kSurface
        $left.Location = [Drawing.Point]::new((U 12), (U 17))
        if ($LeftAction) { $left.Add_Click($LeftAction) }
        $View.Footer.Controls.Add($left)
    }
    $flow = [Windows.Forms.FlowLayoutPanel]::new()
    $flow.FlowDirection = 'LeftToRight'
    $flow.WrapContents = $false
    $flow.AutoSize = $true
    $flow.Dock = 'Right'
    $flow.BackColor = [Drawing.Color]::Transparent
    $flow.Padding = [Windows.Forms.Padding]::new((U 0), (U 12), (U 12), (U 0))
    foreach ($entry in @($Actions)) {
        if ($null -eq $entry) { continue }
        if ($entry -is [Windows.Forms.Button]) { $flow.Controls.Add($entry); continue }
        $button = $entry['Button']
        if ($entry.ContainsKey('Action') -and $null -ne $entry['Action']) { $button.Add_Click($entry['Action']) }
        $flow.Controls.Add($button)
    }
    $View.Footer.Controls.Add($flow)
    $flow.BringToFront()
}

function Get-SetupSignature {
    $stamp = ''
    try { $stamp = [string](Get-Item -LiteralPath $script:settingsFile -ErrorAction Stop).LastWriteTimeUtc.Ticks } catch { }
    return ([string]$script:wizStep + '|' + [string]$script:clientPath + '|' + [string]$script:stagePath + '|' + $stamp)
}

function Show-View([string]$Name) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $script:view = $Name
    # zh: 先重建目标页，再切换可见性，避免旧页已隐藏、新页还在逐控件构建时出现空窗闪跳；
    # 整段挂起布局，构建完成后一次排版，减少切页时的连续重排与闪烁。
    $form.SuspendLayout()
    try {
        $needsBuild = (-not $script:viewBuilt[$Name]) -or $script:viewDirty[$Name]
        if ($needsBuild -and $Name -eq 'setup' -and $script:viewBuilt[$Name]) {
            if ($script:setupSignature -eq (Get-SetupSignature)) { $script:viewDirty[$Name] = $false; $needsBuild = $false }
        }
        if ($needsBuild) {
            switch ($Name) {
                'main' { Update-Main }
                'setup' { Update-Setup }
                'maint' { Update-Maint }
                'uninstall' { Update-Uninstall }
            }
            $script:viewBuilt[$Name] = $true
            $script:viewDirty[$Name] = $false
        }
        Write-Perf '  show/after-build' $sw.ElapsedMilliseconds
        foreach ($key in $views.Keys) { $views[$key].Panel.Visible = ($key -eq $Name) }
        Write-Perf '  show/after-visible' $sw.ElapsedMilliseconds
    } finally {
        $form.ResumeLayout($true)
    }
    Write-Perf '  show/after-resume' $sw.ElapsedMilliseconds
    Write-Perf ('Show-View ' + $Name) $sw.ElapsedMilliseconds
}

function Get-DeployInfo {
    $info = [pscustomobject]@{ Installed = $false; Version = ''; InstalledAt = ''; Count = 0;
        NewFiles = 0; Replaced = 0; Same = 0; BackupRoot = ''; BackupCount = 0; Record = $null }
    try {
        $recordInfo = Test-Dlss5ClientRecord $script:clientPath
    } catch { $recordInfo = $null }
    if ($null -eq $recordInfo) { return $info }
    $info.Installed = $true
    $info.Record = $recordInfo
    $info.Version = [string]$recordInfo.Record.manifest_version
    $installedAt = [string]$recordInfo.Record.installed_utc
    if ($installedAt) {
        try { $info.InstalledAt = ([DateTime]::Parse($installedAt)).ToLocalTime().ToString('yyyy-MM-dd HH:mm') } catch { $info.InstalledAt = $installedAt }
    }
    $entries = @($recordInfo.Record.entries)
    $info.Count = $entries.Count
    $info.NewFiles = @($entries | Where-Object { [string]$_.pre_state -eq 'absent' }).Count
    $info.Replaced = @($entries | Where-Object { [string]$_.pre_state -eq 'present' }).Count
    $info.Same = @($entries | Where-Object { [string]$_.pre_state -eq 'same' }).Count
    $info.BackupRoot = [string]$recordInfo.BackupRoot
    if ($info.BackupRoot -and [IO.Directory]::Exists($info.BackupRoot)) {
        try { $info.BackupCount = @(Get-ChildItem -LiteralPath $info.BackupRoot -Recurse -File -Force).Count } catch { $info.BackupCount = 0 }
    }
    return $info
}

function Update-Main {
    $view = $mainView
    Clear-Panel $view.Extra
    Clear-Panel $view.Body
    $script:logBox = $null
    $script:mainLive = $null
    $script:lastSample = $null
    $script:fpsSource = $null
    $script:fpsOutput = $null
    $swStep = [Diagnostics.Stopwatch]::StartNew()
    $deploy = Get-DeployInfo
    Write-Perf '  main/Get-DeployInfo' $swStep.ElapsedMilliseconds
    $swStep.Restart()
    $clientOk = Test-ClientRoot $script:clientPath
    $status = $null
    if ($clientOk) {
        try {
            $status = Get-Dlss5Status -Client $script:clientPath
            Write-Perf '  main/Get-Dlss5Status' $swStep.ElapsedMilliseconds
            $swStep.Restart()
            $script:status = $status
        } catch {
            $script:status = $null
            Add-Log ('读取客户端状态未完成：' + $_.Exception.Message)
        }
    } else {
        $script:status = $null
    }
    $config = if ($status) { $status.Config } else { $null }
    $running = if ($status) { $status.GameProcesses -gt 0 } else { Test-GameRunning }
    $nrOn = $false
    $fgOn = $false
    $pipeOn = $false
    $presentOn = $false
    if ($config) {
        $nrOn = ([string]$config.NeuralRendering -eq '1')
        $fgOn = ([string]$config.FrameGeneration -eq '1')
        $pipeOn = ([string]$config.PipelineEnabled -eq '1')
        $presentOn = ([string]$config.DetachedPresentation -eq '1')
    }
    $tierInfo = Get-TierInfo

    $hero = New-Card (U 118) $script:kSurface2 $script:kLine
    $caption = New-Label '输出帧率' $script:fSmall $script:kAccent
    $caption.Location = [Drawing.Point]::new((U 16), (U 10))
    $hero.Controls.Add($caption)
    $fps = New-Label '0.0' $script:fGauge $script:kText3
    $fps.Location = [Drawing.Point]::new((U 14), (U 22))
    $hero.Controls.Add($fps)
    $unit = New-Label 'FPS' $script:fSmall $script:kAccent
    $unit.Location = [Drawing.Point]::new(($fps.Right + (U 3)), (($fps.Top + $fps.Height) - $unit.Height - (U 12)))
    $hero.Controls.Add($unit)
    $note = New-Label $(if ($running) { '等待面板上报实时数据…' } else { '启动游戏后显示实时数据' }) $script:fSmall $script:kText2
    $note.Location = [Drawing.Point]::new((U 16), (U 92))
    $hero.Controls.Add($note)
    $gameChip = New-DotChip '' $(if ($running) { '游戏运行中' } else { '游戏未运行' }) $(if ($running) { 'ok' } else { 'muted' })
    $gameChip.Panel.BackColor = $script:kSurface2
    $gameChip.Panel.FillColor = $script:kSurface2
    $gameChip.Panel.Location = [Drawing.Point]::new(($ContentWidth - (U 16) - $gameChip.Panel.Width), (U 12))
    $hero.Controls.Add($gameChip.Panel)
    $start = New-Button $(if ($running) { '▶  打开面板' } else { '▶  启动游戏' }) 'primary' (U 108) (U 30)
    $start.Location = [Drawing.Point]::new(($ContentWidth - (U 16) - (U 108)), (U 46))
    $start.Add_Click({ Invoke-Ui {
        if (-not (Test-ClientRoot $script:clientPath)) { throw '请先在「切换客户端」中选择含 wow.exe 的目录。' }
        if (Test-GameRunning) {
            $logDir = Start-PanelProcess
            Add-Log ('已启动游戏内面板。日志：' + $logDir)
        } else {
            Start-Process -FilePath (Join-Path $script:clientPath 'wow.exe') -WorkingDirectory $script:clientPath
            Add-Log '已启动游戏，等待面板上报实时数据。'
        }
    } })
    $hero.Controls.Add($start)
    $view.Body.Controls.Add($hero)

    $advValues = $null
    try { $advValues = Get-Dlss5AdvancedSettings -Client $script:clientPath } catch { }
    $intensityText = if ($advValues -and $advValues.Intensity) { [string]$advValues.Intensity } else { '—' }
    $profileLine = New-Label ('模型 ' + $tierInfo.Model + ' · 强度 ' + $intensityText + ' · AIO 管线 ' + $(if ($pipeOn) { '已启用' } else { '未启用' })) $script:fSmall $script:kText3
    $profileLine.Margin = [Windows.Forms.Padding]::new((U 0), (U 2), (U 0), (U 2))
    $view.Body.Controls.Add($profileLine)

    $nrText = if ($running) { if ($nrOn) { '运行中' } else { '关闭' } } else { '待机' }
    $fgText = if ($running) { if ($fgOn) { '运行中' } else { '关闭' } } else { '待机' }
    $layerText = if ($running) { if ($presentOn) { '覆盖中' } else { '直通' } } else { '未确认' }
    $nrChip = New-DotChip '画面增强' $nrText $(if ($running -and $nrOn) { 'ok' } else { 'muted' })
    $fgChip = New-DotChip '补帧' $fgText $(if ($running -and $fgOn) { 'ok' } else { 'muted' })
    $layerChip = New-DotChip '图层' $layerText $(if ($running -and $presentOn) { 'ok' } else { 'muted' })
    $tierChip = New-DotChip '档位' $tierInfo.Tier 'ok'
    $chips = [Windows.Forms.FlowLayoutPanel]::new()
    $chips.Width = $ContentWidth
    $chips.Height = (U 26)
    $chips.WrapContents = $false
    $chips.BackColor = [Drawing.Color]::Transparent
    $chips.Margin = [Windows.Forms.Padding]::new((U 0), (U 2), (U 0), (U 4))
    foreach ($chip in @($nrChip, $fgChip, $layerChip, $tierChip)) { $chips.Controls.Add($chip.Panel) }
    $view.Body.Controls.Add($chips)

    $runTitle = New-SectionTitle '运行状态' '实时采样'
    $view.Body.Controls.Add($runTitle)
    $gpuName = ''
    if ($status -and @($status.GraphicsDevices).Count -gt 0) { $gpuName = [string]$status.GraphicsDevices[0].Name }
    $componentDetail = if ($deploy.Installed) { '已部署 ' + $deploy.Version } else { '未安装，切换客户端后进入安装向导' }
    $checksCard = New-Card (U 134) $script:kSurface2 $script:kLineSoft
    $rowY = 4
    $clientRow = New-CheckRow $clientOk '客户端' $(if ($clientOk) { Get-ClientDisplayName $script:clientPath } else { '未选择客户端' })
    $clientRow.Height = (U 42)
    $clientRow.Location = [Drawing.Point]::new(0, $rowY)
    $checksCard.Controls.Add($clientRow)
    $rowY += (U 43)
    $gpuRow = New-CheckRow ([bool]$gpuName) '显卡' $(if ($gpuName) { $gpuName } else { '未检测到显卡' })
    $gpuRow.Height = (U 42)
    $gpuRow.Location = [Drawing.Point]::new(0, $rowY)
    $checksCard.Controls.Add($gpuRow)
    $rowY += (U 43)
    $compRow = New-CheckRow ([bool]$deploy.Installed) '组件' $componentDetail '' $null $(if (-not $deploy.Installed) { (U 120) } else { 0 })
    $compRow.Height = (U 42)
    $compRow.Location = [Drawing.Point]::new(0, $rowY)
    $checksCard.Controls.Add($compRow)
    if (-not $deploy.Installed) {
        $switchBtn = New-Button '切换客户端' 'secondary' (U 104) (U 28)
        $switchBtn.Location = [Drawing.Point]::new(($ContentWidth - (U 16) - (U 104)), ($rowY + (U 4)))
        $switchBtn.Add_Click({ Open-Wizard })
        $checksCard.Controls.Add($switchBtn)
        $switchBtn.BringToFront()
    }
    $checksCard.Visible = -not $running
    $view.Body.Controls.Add($checksCard)
    $stats = New-StatTable
    $stats.Panel.Visible = $running
    $view.Body.Controls.Add($stats.Panel)
    $pipeline = New-Pipeline
    $view.Body.Controls.Add($pipeline.Panel)
    Set-PipelineState $pipeline $running ($running -and $nrOn) ($running -and $nrOn) ($running -and $fgOn)

    $view.Body.Controls.Add((New-SectionTitle '源帧率上限' '写入 WTF\Config.wtf'))
    $capState = Read-FpsCap
    $capEnabled = $capState.Exists
    $capRow = [Windows.Forms.Panel]::new()
    $capRow.Width = $ContentWidth
    $capRow.Height = (U 80)
    $capRow.BackColor = [Drawing.Color]::Transparent
    $capRow.Margin = [Windows.Forms.Padding]::new((U 0), (U 0), (U 0), (U 0))
    $frontLabel = New-Label '前台（0=不限）' $script:fSmall $script:kText3
    $frontLabel.Location = [Drawing.Point]::new((U 0), (U 0))
    $capRow.Controls.Add($frontLabel)
    $capFront = [Windows.Forms.TextBox]::new()
    $capFront.Width = (U 72)
    $capFront.Height = (U 28)
    $capFront.Location = [Drawing.Point]::new((U 0), (U 20))
    $capFront.Text = if ($null -ne $capState.Value) { [string]$capState.Value } else { '0' }
    $capFront.TextAlign = 'Center'
    $capFront.BorderStyle = 'FixedSingle'
    $capFront.BackColor = $script:kSurface2
    $capFront.ForeColor = $script:kText
    $capFront.Font = $script:fBody
    $capFront.Enabled = $capEnabled
    $capRow.Controls.Add($capFront)
    $backLabel = New-Label '后台（0=不限）' $script:fSmall $script:kText3
    $backLabel.Location = [Drawing.Point]::new((U 98), (U 0))
    $capRow.Controls.Add($backLabel)
    $capBack = [Windows.Forms.TextBox]::new()
    $capBack.Width = (U 72)
    $capBack.Height = (U 28)
    $capBack.Location = [Drawing.Point]::new((U 98), (U 20))
    $capBack.Text = if ($null -ne $capState.Back) { [string]$capState.Back } else { '0' }
    $capBack.TextAlign = 'Center'
    $capBack.BorderStyle = 'FixedSingle'
    $capBack.BackColor = $script:kSurface2
    $capBack.ForeColor = $script:kText
    $capBack.Font = $script:fBody
    $capBack.Enabled = $capEnabled
    $capRow.Controls.Add($capBack)
    $capSave = New-Button '保存到 Config.wtf' 'secondary' (U 146) (U 30)
    $capSave.Location = [Drawing.Point]::new(($ContentWidth - (U 146)), (U 20))
    $capSave.Enabled = $capEnabled
    $capRow.Controls.Add($capSave)
    $capNote = New-Label $(if ($capEnabled) { '建议游戏未运行时保存；游戏内输入 /console MaxFPS 30 可即时生效。' } else { '客户端内没有 WTF\Config.wtf，请先启动一次游戏。' }) $script:fSmall $script:kText3
    $capNote.Location = [Drawing.Point]::new((U 0), (U 58))
    $capRow.Controls.Add($capNote)
    $view.Body.Controls.Add($capRow)
    $capSave.Add_Click({ Invoke-Ui {
        if (Test-GameRunning) { throw '请先退出游戏再写入 Config.wtf。' }
        $front = 0
        $back = 0
        if (-not [int]::TryParse($script:mainLive.CapFront.Text.Trim(), [ref]$front)) { throw '前台帧率上限需为整数。' }
        if (-not [int]::TryParse($script:mainLive.CapBack.Text.Trim(), [ref]$back)) { throw '后台帧率上限需为整数。' }
        $backup = Save-FpsCap $front $back
        Add-Log ('已写入源帧率上限：前台 ' + $front + '，后台 ' + $back + '；备份：' + $backup)
        $script:mainLive.CapNote.Text = '已保存；重启游戏后生效。'
    } })

    $script:mainStatus = $null

    $view.Body.Controls.Add((New-SectionTitle '日志' '最近输出'))
    $log = New-LogBox (U 108)
    $log.Box.Location = [Drawing.Point]::new((U 12), (U 10))
    $log.Box.Height = (U 86)
    $view.Body.Controls.Add($log.Panel)
    $script:logBox = $log.Box
    Set-LogBoxText $script:logBox $script:logText

    $actions = [Windows.Forms.FlowLayoutPanel]::new()
    $actions.Width = $ContentWidth
    $actions.Height = (U 36)
    $actions.WrapContents = $false
    $actions.BackColor = [Drawing.Color]::Transparent
    $actions.Margin = [Windows.Forms.Padding]::new((U 0), (U 10), (U 0), (U 8))
    $check = New-Button '检查状态' 'secondary' (U 96) (U 32)
    $check.Add_Click({ Invoke-Ui { Add-Log '已刷新客户端状态。'; Show-View 'main' } })
    $panelLog = New-Button '打开悬浮面板日志' 'secondary' (U 132) (U 32)
    $panelLog.Add_Click({ Invoke-Ui {
        if (-not $script:lastPanelLog -or -not [IO.Directory]::Exists($script:lastPanelLog)) { throw '本次窗口尚未启动面板。' }
        $lines = @()
        foreach ($name in @('stdout.log', 'stderr.log')) {
            $path = Join-Path $script:lastPanelLog $name
            if ([IO.File]::Exists($path)) { $lines += @(Get-Content -LiteralPath $path -Tail 40 -Encoding UTF8 -ErrorAction Stop) }
        }
        Add-Log $(if ($lines.Count) { $lines -join "`r`n" } else { '面板暂无日志。' })
    } })
    $watchLog = New-Button '打开载体日志' 'secondary' (U 122) (U 32)
    $watchLog.Add_Click({ Invoke-Ui {
        if (-not $script:lastWatchLog -or -not [IO.Directory]::Exists($script:lastWatchLog)) { throw '本次窗口尚未启动助手。' }
        $lines = @()
        foreach ($name in @('stdout.log', 'stderr.log')) {
            $path = Join-Path $script:lastWatchLog $name
            if ([IO.File]::Exists($path)) { $lines += @(Get-Content -LiteralPath $path -Tail 40 -Encoding UTF8 -ErrorAction Stop) }
        }
        Add-Log $(if ($lines.Count) { $lines -join "`r`n" } else { '助手暂无日志。' })
    } })
    $actions.Controls.Add($check)
    $actions.Controls.Add($panelLog)
    $actions.Controls.Add($watchLog)
    $view.Body.Controls.Add($actions)

    Write-Perf '  main/build' $swStep.ElapsedMilliseconds
    $script:mainLive = [pscustomobject]@{ GameChip = $gameChip; Start = $start; Fps = $fps; Unit = $unit; Note = $note;
        Chips = [pscustomobject]@{ Nr = $nrChip; Fg = $fgChip; Layer = $layerChip; Tier = $tierChip };
        ChipsState = @{ Nr = $nrOn; Fg = $fgOn; Pipeline = $pipeOn; Present = $presentOn };
        RunTitle = $runTitle; Stats = $stats; Checks = $checksCard; Pipeline = $pipeline;
        CapNote = $capNote; CapFront = $capFront; CapBack = $capBack }

    Set-Footer $view '' $null @(
        @{ Button = (New-Button '切换客户端' 'ghost' (U 96) (U 30)); Action = { Open-Wizard } },
        @{ Button = (New-Button '维护' 'ghost' (U 64) (U 30)); Action = { if ((Get-DeployInfo).Installed) { Show-View 'maint' } else { Add-Log '当前客户端还没有安装记录。' } } }
    )
    $pathLabel = New-Label $(if ($script:clientPath) { '客户端：' + $script:clientPath } else { '未选择客户端' }) $script:fSmall $script:kText3
    $pathLabel.AutoSize = $false
    $pathLabel.AutoEllipsis = $true
    $pathLabel.Width = (U 396)
    $pathLabel.Height = (U 18)
    $pathLabel.Location = [Drawing.Point]::new((U 16), (U 8))
    $view.Footer.Controls.Add($pathLabel)
    $pathLabel.BringToFront()
    $footerState = $(if ($deploy.Installed) { '已部署 ' + $deploy.Version } else { '未安装' }) + '  ·  ' + $(if ($running) { '游戏运行中' } else { '游戏未运行' })
    $stateLabel = New-Label $footerState $script:fSmall $script:kText3
    $stateLabel.Location = [Drawing.Point]::new((U 16), (U 27))
    $view.Footer.Controls.Add($stateLabel)
    $stateLabel.BringToFront()
    if ($null -ne $script:pathTip) { try { $script:pathTip.Dispose() } catch { } }
    $script:pathTip = [Windows.Forms.ToolTip]::new()
    $script:pathTip.SetToolTip($pathLabel, $(if ($script:clientPath) { $script:clientPath } else { '尚未选择客户端' }))
    $view.Body.AutoScrollPosition = [Drawing.Point]::new((U 0), (U 0))
}
function Update-RuntimeState {
    if ($script:view -ne 'main' -or $null -eq $script:mainLive) { return }
    $live = $script:mainLive
    try {
        if ($live.Start.IsDisposed) { return }
        $hostId = Get-AioHostId
        $tel = Read-TelemetrySample $hostId
        $running = ($hostId -gt 0) -or (Test-GameRunning)
        Set-DotChip $live.GameChip $(if ($running) { '游戏运行中' } else { '游戏未运行' }) $(if ($running) { 'ok' } else { 'muted' })
        $live.GameChip.Panel.Location = [Drawing.Point]::new(($ContentWidth - (U 16) - $live.GameChip.Panel.Width), (U 12))
        $live.Start.Text = if ($running) { '▶  打开面板' } else { '▶  启动游戏' }

        if ($hostId -le 0) {
            $script:fpsOutput = $null
            $script:fpsSource = $null
            $script:lastSample = $null
        }
        $hasTel = ($null -ne $tel -and $tel.Available)
        if ($hasTel) {
            $script:fpsSource = [double]$tel.SourceFps
            $now = [Diagnostics.Stopwatch]::GetTimestamp() / [double][Diagnostics.Stopwatch]::Frequency
            if ($null -ne $script:lastSample) {
                $delta = $now - $script:lastSample.At
                if ($delta -ge 0.5) {
                    $output = ([double]$tel.FramesPresented - $script:lastSample.Frames) / $delta
                    if ($output -ge 0 -and $output -lt 1000) { $script:fpsOutput = $output }
                    $script:lastSample = @{ At = $now; Frames = [double]$tel.FramesPresented }
                }
            } else {
                $script:lastSample = @{ At = $now; Frames = [double]$tel.FramesPresented }
            }
            if ($null -ne $script:fpsOutput) {
                $live.Fps.Text = '{0:0.0}' -f $script:fpsOutput
                $live.Fps.ForeColor = $script:kText
            }
            if ($null -ne $script:fpsOutput -and $null -ne $script:fpsSource -and $script:fpsSource -gt 0) {
                $live.Note.Text = ('源 {0:0.0}  →  输出 {1:0.0}  ·  增益 {2:0.00}×' -f $script:fpsSource, $script:fpsOutput, ($script:fpsOutput / $script:fpsSource))
            } else {
                $live.Note.Text = '实时采样 · 每秒刷新'
            }
        } else {
            $live.Fps.Text = '0.0'
            $live.Fps.ForeColor = $script:kText3
            $live.Note.Text = if ($running) { '等待面板上报实时数据…' } else { '启动游戏后显示实时数据' }
        }

        $nrOn = if ($hasTel) { [bool]$tel.NrEnabled } else { [bool]$live.ChipsState.Nr }
        $fgOn = if ($hasTel) { [bool]$tel.FrameGeneration } else { [bool]$live.ChipsState.Fg }
        $presentOn = if ($hasTel) { [bool]$tel.ProcessedOutputSelected } else { [bool]$live.ChipsState.Present }
        $pipeOn = [bool]$live.ChipsState.Pipeline
        if ($running) {
            Set-DotChip $live.Chips.Nr $(if ($nrOn) { '运行中' } else { '关闭' }) $(if ($nrOn) { 'ok' } else { 'muted' })
            $fgText = if ($fgOn) { '运行中' } else { '关闭' }
            if ($fgOn -and $hasTel -and $tel.FgFrames -gt 0) { $fgText = '已插帧 ' + ('{0:N0}' -f $tel.FgFrames) }
            Set-DotChip $live.Chips.Fg $fgText $(if ($fgOn) { 'ok' } else { 'muted' })
            Set-DotChip $live.Chips.Layer $(if ($presentOn) { '覆盖中' } else { '直通' }) $(if ($presentOn) { 'ok' } else { 'muted' })
        } else {
            Set-DotChip $live.Chips.Nr '待机' 'muted'
            Set-DotChip $live.Chips.Fg '待机' 'muted'
            Set-DotChip $live.Chips.Layer '未确认' 'muted'
        }

        Set-SectionTitle $live.RunTitle '运行状态' '实时采样'
        $live.Stats.Panel.Visible = $running
        $live.Checks.Visible = -not $running
        Set-PipelineState $live.Pipeline $running ($running -and $nrOn) ($running -and $nrOn) ($running -and $fgOn)

        $values = @{
            resIn = '—'; resOut = '—'; scale = '—'
            latency = '—'; vram = '—'; gpu = '—'
            nrFrames = '—'; srFrames = '—'; fgFrames = '—'
        }
        if ($running -and $hasTel) {
            if ($tel.InputWidth -gt 0 -and $tel.InputHeight -gt 0) { $values.resIn = '{0}×{1}' -f $tel.InputWidth, $tel.InputHeight }
            if ($tel.OutputWidth -gt 0 -and $tel.OutputHeight -gt 0) { $values.resOut = '{0}×{1}' -f $tel.OutputWidth, $tel.OutputHeight }
            if ($tel.InputWidth -gt 0 -and $tel.OutputWidth -gt 0) { $values.scale = '{0:0.00}×' -f ($tel.OutputWidth / [double]$tel.InputWidth) }
            if ($tel.GpuTotalUs -gt 0) { $values.latency = '{0:0.0} ms' -f ($tel.GpuTotalUs / 1000.0) }
            elseif ($tel.SourceFrameAverageUs -gt 0) { $values.latency = '{0:0.0} ms' -f ($tel.SourceFrameAverageUs / 1000.0) }
            $values.nrFrames = '{0:N0}' -f $tel.NrFrames
            $values.srFrames = '{0:N0}' -f $tel.SrFrames
            $values.fgFrames = '{0:N0}' -f $tel.FgFrames
            $gpuStats = Get-GpuStats
            if ($null -ne $gpuStats) {
                if ($null -ne $gpuStats.Vram) { $values.vram = '{0:0.0} GB' -f $gpuStats.Vram }
                if ($null -ne $gpuStats.Load) { $values.gpu = '{0:0}%' -f $gpuStats.Load }
            }
        }
        Set-StatTable $live.Stats $values
        $live.Fps.PerformLayout()
        $live.Unit.Location = [Drawing.Point]::new(($live.Fps.Right + (U 3)), (($live.Fps.Top + $live.Fps.Height) - $live.Unit.Height - (U 12)))
    } catch { }
}
function Refresh-Status {
    try {
        $script:status = Get-Dlss5Status -Client $script:clientPath
        Add-Log ("客户端：" + $script:status.GameIdentity + "；游戏进程 " + $script:status.GameProcesses + "；载体进程 " + $script:status.HostProcesses)
        Add-Log ("安装记录：" + $script:status.InstallRecord)
        foreach ($file in @($script:status.Files)) {
            if ($file.Status -ne '与候选一致') { Add-Log ("文件 " + $file.Name + "：" + $file.Status) }
        }
        if ($null -ne $script:mainStatus) {
            $script:mainStatus.Game.Text = if ($script:status.GameProcesses -gt 0) { "运行中（$($script:status.GameProcesses)）" } else { '未运行' }
            $script:mainStatus.Host.Text = if ($script:status.HostProcesses -gt 0) { "运行中（$($script:status.HostProcesses)）" } else { '未运行' }
        }
    } catch {
        Add-Log ('检查状态未完成：' + $_.Exception.Message)
    }
}

function Test-ClientDir([string]$Path) {
    if (-not $Path) { return '未选择客户端目录' }
    if (-not [IO.Directory]::Exists($Path)) { return '目录不存在' }
    if (-not [IO.File]::Exists((Join-Path $Path 'wow.exe'))) { return '目录下没有 wow.exe' }
    return '已识别客户端'
}

function Open-Wizard {
    $script:wizStep = 1
    $script:preview = $null
    $script:installResult = $null
    $script:installError = $null
    $script:viewDirty['setup'] = $true
    Show-View 'setup'
}

function Go-SetupStep([int]$Step) {
    $script:wizStep = $Step
    Update-Setup
}

function New-WizardSteps([int]$Current) {
    $names = @('选择客户端', '环境检查', '文件清单', '安装', '完成')
    $row = [Windows.Forms.Panel]::new()
    $row.Width = $ContentWidth
    $row.Height = (U 24)
    $row.BackColor = [Drawing.Color]::Transparent
    $widths = @()
    foreach ($name in $names) {
        $probe = New-Label $name $script:fSmall $script:kText3
        $widths += ((U 23) + $probe.PreferredWidth)
    }
    $total = ($widths | Measure-Object -Sum).Sum
    $gap = [Math]::Max((U 8), [int](($ContentWidth - $total) / 4))
    $x = 0
    for ($i = 0; $i -lt $names.Count; $i++) {
        $n = $i + 1
        $done = $n -lt $Current
        $active = $n -eq $Current
        $circle = [Windows.Forms.Panel]::new()
        $circle.Location = [Drawing.Point]::new($x, (U 2))
        $circle.Size = [Drawing.Size]::new((U 18), (U 18))
        $circle.BackColor = if ($done) { C '1D2A22' } elseif ($active) { C '33290F' } else { $script:kSurface2 }
        $circle.Tag = if ($done) { C '3C5A46' } elseif ($active) { C '5A4A2E' } else { $script:kLineSoft }
        $circle.Add_Paint({
            param($sender, $eventArgs)
            $eventArgs.Graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
            $pen = [Drawing.Pen]::new([Drawing.Color]$sender.Tag, 1)
            $eventArgs.Graphics.DrawEllipse($pen, 0, 0, ($sender.Width - 1), ($sender.Height - 1))
            $pen.Dispose()
        })
        Set-Rounded $circle (U 9)
        $glyph = New-Label $(if ($done) { '✓' } else { [string]$n }) (New-UiFont 'Microsoft YaHei UI' 10) $(if ($done) { $script:kJade } elseif ($active) { $script:kAccentBright } else { $script:kText3 })
        $glyph.AutoSize = $false
        $glyph.TextAlign = 'MiddleCenter'
        $glyph.Size = [Drawing.Size]::new((U 18), (U 18))
        $glyph.Location = [Drawing.Point]::new(0, 0)
        $circle.Controls.Add($glyph)
        $row.Controls.Add($circle)
        $label = New-Label $names[$i] $script:fSmall $(if ($done) { $script:kText2 } elseif ($active) { $script:kText } else { $script:kText3 })
        $label.Location = [Drawing.Point]::new(($x + (U 23)), (U 2))
        $row.Controls.Add($label)
        $x += $widths[$i]
        if ($i -lt $names.Count - 1) {
            $line = [Windows.Forms.Panel]::new()
            $line.Location = [Drawing.Point]::new(($x + (U 4)), (U 11))
            $line.Size = [Drawing.Size]::new(($gap - (U 8)), 1)
            $line.BackColor = if ($done) { C '3C5A46' } else { $script:kLineSoft }
            $row.Controls.Add($line)
            $x += $gap
        }
    }
    return $row
}

function Update-Setup {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $view = $setupView
    Clear-Panel $view.Extra
    Clear-Panel $view.Body
    $view.Extra.Height = (U 26)
    $view.Extra.Controls.Add((New-WizardSteps $script:wizStep))

    switch ($script:wizStep) {
        1 { Build-SetupStep1 $view }
        2 { Build-SetupStep2 $view }
        3 { Build-SetupStep3 $view }
        4 { Build-SetupStep4 $view }
        5 { Build-SetupStep5 $view }
    }
    $script:setupSignature = Get-SetupSignature
    Write-Perf ('  setup/build step ' + $script:wizStep) $sw.ElapsedMilliseconds
}

function Build-SetupStep1($View) {
    $View.Body.Controls.Add((New-SectionTitle '客户端目录' '已记住上次选择'))
    $clientCard = New-Card (U 56) $script:kSurface2 $script:kLine
    $folder = New-Label '▤' $script:fBold $script:kAccent
    $folder.Location = [Drawing.Point]::new((U 16), (U 16))
    $clientCard.Controls.Add($folder)
    $pathLabel = New-Label $(if ($script:clientPath) { $script:clientPath } else { '尚未选择客户端目录' }) $script:fBody $(if ($script:clientPath) { $script:kText } else { $script:kText3 })
    $pathLabel.AutoSize = $false
    $pathLabel.AutoEllipsis = $true
    $pathLabel.TextAlign = 'MiddleLeft'
    $pathLabel.Location = [Drawing.Point]::new((U 38), (U 15))
    $pathLabel.Size = [Drawing.Size]::new((U 420), (U 26))
    $clientCard.Controls.Add($pathLabel)
    $browse = New-Button '浏览' 'secondary' (U 78) (U 28)
    $browse.Location = [Drawing.Point]::new((U 474), (U 14))
    $browse.Add_Click({
        $dialog = [Windows.Forms.FolderBrowserDialog]::new()
        try {
            if ([IO.Directory]::Exists($script:clientPath)) { $dialog.SelectedPath = $script:clientPath }
            if ($dialog.ShowDialog($form) -eq [Windows.Forms.DialogResult]::OK) {
                $script:clientPath = $dialog.SelectedPath
                Save-ConsoleSettings
                $script:wizStep = 1
                Update-Setup
            }
        } finally { $dialog.Dispose() }
    })
    $clientCard.Controls.Add($browse)
    $View.Body.Controls.Add($clientCard)

    $hint = New-Label '需要包含 wow.exe 与 Data 目录的客户端根目录。控制台可独立于客户端安装。' $script:fSmall $script:kText3 (U 520)
    $hint.Margin = [Windows.Forms.Padding]::new((U 0), (U 8), (U 0), (U 0))
    $View.Body.Controls.Add($hint)

    $clientState = Test-ClientDir $script:clientPath
    $clientOk = ($clientState -eq '已识别客户端')
    $gameVer = ''
    try {
        if ($script:clientPath) {
            $info = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $script:clientPath 'wow.exe'))
            $gameVer = [string]$info.FileVersion
        }
    } catch { }
    $detail = if ($clientOk) { if ($gameVer) { $gameVer + ' · 32 位' } else { 'wow.exe 已找到' } } else { $clientState }
    $checkCard = New-Card (U 50) $script:kSurface2 $script:kLineSoft
    $row = New-CheckRow $clientOk '已识别客户端' $detail $(if ($clientOk) { '就绪' } else { '未就绪' })
    $row.Location = [Drawing.Point]::new((U 0), (U 3))
    $checkCard.Controls.Add($row)
    $View.Body.Controls.Add($checkCard)

    $recent = @()
    try {
        $settings = Read-ConsoleSettings
        $recent = @($settings.Recent)
    } catch { }
    if ($script:clientPath) {
        $hasCurrent = $false
        foreach ($item in $recent) { if ($item.Path -and $item.Path.Equals($script:clientPath, [StringComparison]::OrdinalIgnoreCase)) { $hasCurrent = $true } }
        if (-not $hasCurrent) {
            $recent = @([pscustomobject]@{ Path = $script:clientPath; Name = (Get-ClientDisplayName $script:clientPath) }) + $recent
        }
    }
    if ($recent.Count -gt 0) {
        $recentLabel = New-Label '最近使用' $script:fSmall $script:kText3
        $recentLabel.Margin = [Windows.Forms.Padding]::new((U 0), (U 16), (U 0), (U 8))
        $View.Body.Controls.Add($recentLabel)
        $index = 0
        foreach ($item in $recent) {
            if (-not $item.Path -or -not [IO.Directory]::Exists($item.Path)) { continue }
            $index++
            if ($index -gt 3) { break }
            $isCurrent = ($script:clientPath -and $item.Path.Equals($script:clientPath, [StringComparison]::OrdinalIgnoreCase))
            $version = ''
            try { $record = Test-Dlss5ClientRecord $item.Path; if ($null -ne $record) { $version = [string]$record.Record.manifest_version } } catch { }
            $card = New-Card (U 52) $(if ($isCurrent) { C '28200F' } else { $script:kSurface2 }) $(if ($isCurrent) { C '5A4A2E' } else { $script:kLineSoft })
            $card.Margin = [Windows.Forms.Padding]::new((U 0), (U 0), (U 0), (U 8))
            $name = New-Label $(if ($item.Name) { $item.Name } else { Get-ClientDisplayName $item.Path }) $script:fBody $script:kText
            $name.AutoSize = $false
            $name.UseCompatibleTextRendering = $true
            $name.Location = [Drawing.Point]::new((U 14), (U 8))
            $name.Size = [Drawing.Size]::new((U 400), (U 18))
            $card.Controls.Add($name)
            $sub = New-Label $item.Path $script:fSmall $script:kText3
            $sub.AutoSize = $false
            $sub.UseCompatibleTextRendering = $true
            $sub.AutoEllipsis = $true
            $sub.TextAlign = 'MiddleLeft'
            $sub.Location = [Drawing.Point]::new((U 14), (U 27))
            $sub.Size = [Drawing.Size]::new((U 464), (U 16))
            $card.Controls.Add($sub)
            if ($isCurrent -or $version) {
                $tag = New-Label $(if ($isCurrent) { '当前' } elseif ($version) { '已安装 ' + $version } else { '' }) $script:fSmall $(if ($isCurrent) { $script:kJadeText } else { $script:kText3 })
                $tag.AutoSize = $false
                $tag.TextAlign = 'MiddleRight'
                $tag.Size = [Drawing.Size]::new((U 84), (U 18))
                $tag.Location = [Drawing.Point]::new((U 468), (U 8))
                $card.Controls.Add($tag)
            }
            foreach ($target in @($card, $name, $sub)) {
                $target.Cursor = [Windows.Forms.Cursors]::Hand
                $target.Add_Click(({
                    $script:clientPath = $item.Path
                    Save-ConsoleSettings
                    $script:wizStep = 1
                    Update-Setup
                }).GetNewClosure())
            }
            $View.Body.Controls.Add($card)
        }
    }

    $View.Body.Controls.Add((New-SectionTitle '组件来源' '发行包或暂存目录，含清单文件'))
    $stageCard = New-Card (U 56) $script:kSurface2 $script:kLine
    $stageLabel = New-Label $(if ($script:stagePath) { $script:stagePath } else { '尚未选择组件来源' }) $script:fBody $(if ($script:stagePath) { $script:kText } else { $script:kText3 })
    $stageLabel.AutoSize = $false
    $stageLabel.AutoEllipsis = $true
    $stageLabel.TextAlign = 'MiddleLeft'
    $stageLabel.Location = [Drawing.Point]::new((U 16), (U 15))
    $stageLabel.Size = [Drawing.Size]::new((U 446), (U 26))
    $stageCard.Controls.Add($stageLabel)
    $stageBrowse = New-Button '浏览' 'secondary' (U 78) (U 28)
    $stageBrowse.Location = [Drawing.Point]::new((U 474), (U 14))
    $stageBrowse.Add_Click({
        $dialog = [Windows.Forms.FolderBrowserDialog]::new()
        try {
            if ([IO.Directory]::Exists($script:stagePath)) { $dialog.SelectedPath = $script:stagePath }
            if ($dialog.ShowDialog($form) -eq [Windows.Forms.DialogResult]::OK) {
                $script:stagePath = $dialog.SelectedPath
                Save-ConsoleSettings
                $script:wizStep = 1
                Update-Setup
            }
        } finally { $dialog.Dispose() }
    })
    $stageCard.Controls.Add($stageBrowse)
    $View.Body.Controls.Add($stageCard)
    $stageState = if (-not $script:stagePath) { '未选择组件来源' } elseif ([IO.Directory]::Exists($script:stagePath)) { '来源目录存在' } else { '来源目录不存在' }
    $View.Body.Controls.Add((New-CheckRow ($stageState -eq '来源目录存在') '组件来源' $stageState '就绪' ))

    Set-Footer $View '返回控制台' { Show-View 'main' } @(
        @{ Button = (New-Button '开始检查' 'primary' (U 112) (U 32)); Action = { Start-Precheck } }
    )
}
function Start-Precheck {
    if ((Test-ClientDir $script:clientPath) -ne '已识别客户端') { Add-Log '请选择包含 wow.exe 的客户端目录。'; return }
    if (-not $script:stagePath -or -not [IO.Directory]::Exists($script:stagePath)) { Add-Log '请选择存在的组件来源目录。'; return }
    $form.UseWaitCursor = $true
    [Windows.Forms.Application]::DoEvents()
    try {
        $script:preview = Get-Dlss5InstallPreview -Client $script:clientPath -Stage $script:stagePath
        $script:checks = @()
        $clientOk = $true
        $gameIdentity = ''
        try {
            $script:status = Get-Dlss5Status -Client $script:clientPath
            $gameIdentity = [string]$script:status.GameIdentity
            $clientOk = $gameIdentity -like '存在*x86*'
        } catch { $clientOk = $false }
        $script:checks += [pscustomobject]@{ Ok = $clientOk; Text = '客户端可识别'; Detail = $(if ($gameIdentity) { $gameIdentity } else { 'wow.exe 缺失或不是 32 位' }) }
        $idle = $true
        $procDetail = '游戏与载体均未运行'
        if ($null -ne $script:status) {
            $idle = ($script:status.GameProcesses -eq 0 -and $script:status.HostProcesses -eq 0)
            if (-not $idle) { $procDetail = "游戏进程 $($script:status.GameProcesses) 个，载体进程 $($script:status.HostProcesses) 个" }
        }
        $script:checks += [pscustomobject]@{ Ok = $idle; Text = '游戏与载体已退出'; Detail = $procDetail }
        $issues = @($script:preview.Items | Where-Object { $null -ne $_.Issue })
        $script:checks += [pscustomobject]@{ Ok = ($issues.Count -eq 0); Text = '组件来源完整'; Detail = if ($issues.Count -eq 0) { "版本 $($script:preview.ManifestVersion)，$($script:preview.Items.Count) 个成员" } else { ($issues | ForEach-Object { $_.Target + '：' + $_.Issue }) -join '；' } }
        $recordInfo = Test-Dlss5ClientRecord $script:clientPath
        $recordOk = ($null -eq $recordInfo)
        $recordDetail = if ($null -eq $recordInfo) { '首次安装，没有旧记录' } else { "已有安装记录（$(@($recordInfo.Record.entries).Count) 项），将按新清单更新" }
        $script:checks += [pscustomobject]@{ Ok = $recordOk; Text = '安装记录'; Detail = $recordDetail }
        $script:wizStep = 2
        Update-Setup
    } catch {
        $script:preview = $null
        $script:checks = @([pscustomobject]@{ Ok = $false; Text = '预检未完成'; Detail = $_.Exception.Message })
        Add-Log ('预检未完成：' + $_.Exception.Message)
        $script:wizStep = 2
        Update-Setup
    } finally {
        $form.UseWaitCursor = $false
    }
}

function Build-SetupStep2($View) {
    $ok = ($null -ne $script:preview)
    $View.Body.Controls.Add((New-SectionTitle '环境检查' '只读，不改动客户端'))
    $card = New-Card ($(if ($script:checks.Count -gt 0) { $script:checks.Count * (U 44) + (U 8) } else { (U 52) })) $script:kSurface2 $script:kLineSoft
    $y = 4
    foreach ($check in @($script:checks)) {
        $row = New-CheckRow ([bool]$check.Ok) ([string]$check.Text) ([string]$check.Detail) $(if ($check.Ok) { '通过' } else { '注意' })
        $row.Location = [Drawing.Point]::new((U 0), $y)
        $card.Controls.Add($row)
        $y += $row.Height + (U 1)
    }
    if ($script:checks.Count -eq 0) {
        $empty = New-Label '预检没有完成，请返回上一步重试。' $script:fBody $script:kWarn
        $empty.Location = [Drawing.Point]::new((U 16), (U 16))
        $card.Controls.Add($empty)
    }
    $View.Body.Controls.Add($card)

    if ($null -ne $script:preview) {
        $summary = New-Label ("可安装：" + $(if ($script:preview.CanInstall) { '是' } else { '否，请先处理冲突' }) + "　新增 " + $script:preview.NewFiles + " · 修改 " + $script:preview.Replaced + " · 跳过 " + $script:preview.SameFiles) $script:fSmall $(if ($script:preview.CanInstall) { $script:kText2 } else { $script:kWarn }) (U 520)
        $summary.Margin = [Windows.Forms.Padding]::new((U 0), (U 8), (U 0), (U 0))
        $View.Body.Controls.Add($summary)
    }

    $canNext = ($null -ne $script:preview -and $script:preview.CanInstall)
    $next = New-Button '查看清单' 'primary' (U 112) (U 32)
    if (-not $canNext) { $next.Enabled = $false; $next.BackColor = $script:kSurface3; $next.ForeColor = $script:kText3 }
    Set-Footer $View '返回控制台' { Show-View 'main' } @(
        @{ Button = (New-Button '上一步' 'secondary' (U 96) (U 32)); Action = { Go-SetupStep 1 } },
        @{ Button = $next; Action = { Go-SetupStep 3 } }
    )
}

function Build-SetupStep3($View) {
    $View.Body.Controls.Add((New-SectionTitle '文件清单' $(if ($script:preview) { "版本 $($script:preview.ManifestVersion)" } else { '' })))
    if ($null -eq $script:preview) {
        $View.Body.Controls.Add((New-Label '没有可用的预检结果，请返回上一步。' $script:fBody $script:kWarn))
        Set-Footer $View '返回控制台' { Show-View 'main' } @(@{ Button = (New-Button '上一步' 'secondary' (U 96) (U 32)); Action = { Go-SetupStep 2 } })
        return
    }
    $groups = @(
        @{ Title = '新增'; Note = '客户端原本没有，卸载时删除'; Filter = { [string]$_.PreState -eq 'absent' } },
        @{ Title = '修改'; Note = '备份后合并写入，卸载时还原'; Filter = { [string]$_.PreState -eq 'present' } },
        @{ Title = '跳过'; Note = '内容一致或可选缺失，不动'; Filter = { -not ($_.PreState -in @('absent', 'present')) } }
    )
    foreach ($group in $groups) {
        $items = @($script:preview.Items | Where-Object $group.Filter)
        if ($items.Count -eq 0) { continue }
        $View.Body.Controls.Add((New-SectionTitle $group.Title ($items.Count.ToString() + ' 项 · ' + $group.Note)))
        $color = switch ($group.Title) { '新增' { $script:kJade } '修改' { $script:kAccent } default { $script:kText3 } }
        $card = New-Card ($items.Count * (U 40) + (U 8)) $script:kSurface2 $script:kLineSoft
        $y = 4
        foreach ($item in $items) {
            $tag = switch ($group.Title) { '新增' { '新增' } '修改' { '备份后修改' } default { '跳过' } }
            $note = if ([string]$item.PreState -eq 'missing-optional') { '可选组件缺失' } else { '' }
            $row = New-FileRow ([string]$item.Target) $tag $color $note
            $row.Location = [Drawing.Point]::new((U 0), $y)
            $card.Controls.Add($row)
            $y += $row.Height + (U 1)
        }
        $View.Body.Controls.Add($card)
    }

    $issues = @($script:preview.Items | Where-Object { $null -ne $_.Issue })
    if ($issues.Count -gt 0) {
        $View.Body.Controls.Add((New-SectionTitle '冲突' ($issues.Count.ToString() + ' 项需要处理')))
        $card = New-Card ($issues.Count * (U 40) + (U 8)) $script:kSurface2 $script:kWarn
        $y = 4
        foreach ($item in $issues) {
            $row = New-FileRow ([string]$item.Target) '冲突' $script:kWarn ([string]$item.Issue)
            $row.Location = [Drawing.Point]::new((U 0), $y)
            $card.Controls.Add($row)
            $y += $row.Height + (U 1)
        }
        $View.Body.Controls.Add($card)
    }

    $backupNote = New-Card (U 44) $script:kSurface2 $script:kLineSoft
    $note = New-Label '修改的原件备份到客户端内 host64\dlss5-backup，卸载时按安装记录还原。' $script:fSmall $script:kText3
    $note.Location = [Drawing.Point]::new((U 16), (U 13))
    $backupNote.Controls.Add($note)
    $View.Body.Controls.Add($backupNote)

    $start = New-Button '开始安装' 'primary' (U 112) (U 32)
    if (-not $script:preview.CanInstall) { $start.Enabled = $false; $start.BackColor = $script:kSurface3; $start.ForeColor = $script:kText3 }
    Set-Footer $View '返回控制台' { Show-View 'main' } @(
        @{ Button = (New-Button '上一步' 'secondary' (U 96) (U 32)); Action = { Go-SetupStep 2 } },
        @{ Button = $start; Action = { Start-Install } }
    )
}

function Start-Install {
    if ($null -eq $script:preview -or -not $script:preview.CanInstall) { Add-Log '预检未通过，不能安装。'; return }
    $script:installResult = $null
    $script:installError = $null
    $script:wizStep = 4
    Update-Setup
    $form.UseWaitCursor = $true
    [Windows.Forms.Application]::DoEvents()
    try {
        $script:installResult = Invoke-Dlss5Install -Client $script:clientPath -Stage $script:stagePath
        if ($script:installResult.Status -eq 'already_current') {
            Add-Log '客户端已是最新，无需写入。'
        } else {
            Add-Log ("安装完成：写入 " + $script:installResult.Count + " 项。")
            Add-Log ('安装记录：' + $script:installResult.Record)
            Add-Log ('事务记录：' + $script:installResult.Operation)
        }
    } catch {
        $script:installError = $_.Exception.Message
        Add-Log ('安装未完成：' + $script:installError)
    } finally {
        $form.UseWaitCursor = $false
        $script:wizStep = 5
        Update-Setup
    }
}

function Build-SetupStep4($View) {
    $View.Body.Controls.Add((New-SectionTitle '正在安装' '正在写入客户端'))
    $card = New-Card (U 96) $script:kSurface2 $script:kLineSoft
    $label = New-Label '正在安装…' $script:fBody $script:kAccent
    $label.Location = [Drawing.Point]::new((U 18), (U 18))
    $card.Controls.Add($label)
    $note = New-Label '修改前先备份原件；完成后回读每个文件校验哈希。' $script:fSmall $script:kText3
    $note.Location = [Drawing.Point]::new((U 18), (U 44))
    $card.Controls.Add($note)
    $bar = [Windows.Forms.Panel]::new()
    $bar.Location = [Drawing.Point]::new((U 18), (U 70))
    $bar.Width = $ContentWidth - 36
    $bar.Height = (U 4)
    $bar.BackColor = $script:kLineSoft
    $fill = [Windows.Forms.Panel]::new()
    $fill.Location = [Drawing.Point]::new((U 0), (U 0))
    $fill.Width = [int](($ContentWidth - (U 36)) * 0.6)
    $fill.Height = (U 4)
    $fill.BackColor = $script:kAccent
    $bar.Controls.Add($fill)
    $card.Controls.Add($bar)
    $View.Body.Controls.Add($card)
    Set-Footer $View '返回控制台' { Show-View 'main' } @()
}

function Build-SetupStep5($View) {
    $done = ($null -ne $script:installResult -and $null -eq $script:installError)
    $deploy = Get-DeployInfo
    $hero = [Windows.Forms.Panel]::new()
    $hero.Width = $ContentWidth
    $hero.Height = (U 196)
    $hero.BackColor = [Drawing.Color]::Transparent
    $hero.Margin = [Windows.Forms.Padding]::new((U 0), (U 4), (U 0), (U 6))
    $badge = [Windows.Forms.Panel]::new()
    $badge.Size = [Drawing.Size]::new((U 52), (U 52))
    $badge.Location = [Drawing.Point]::new([int](($ContentWidth - (U 52)) / 2), (U 6))
    $badge.BackColor = if ($done) { C '1D2A22' } else { C '2E2116' }
    $badge.Tag = if ($done) { C '3C5A46' } else { C '6A4A32' }
    $badge.Add_Paint({
        param($sender, $eventArgs)
        $eventArgs.Graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $pen = [Drawing.Pen]::new([Drawing.Color]$sender.Tag, 1)
        $eventArgs.Graphics.DrawEllipse($pen, 0, 0, ($sender.Width - 1), ($sender.Height - 1))
        $pen.Dispose()
    })
    Set-Rounded $badge (U 26)
    $glyph = New-Label $(if ($done) { '✓' } else { '!' }) (New-UiFont 'Microsoft YaHei UI' 18 ([Drawing.FontStyle]::Bold)) $(if ($done) { $script:kJade } else { $script:kWarn })
    $glyph.AutoSize = $false
    $glyph.TextAlign = 'MiddleCenter'
    $glyph.Size = [Drawing.Size]::new((U 52), (U 52))
    $glyph.Location = [Drawing.Point]::new(0, 0)
    $badge.Controls.Add($glyph)
    $hero.Controls.Add($badge)
    $title = New-Label $(if ($done) { '安装完成' } else { '安装未完成' }) $script:fTitle $(if ($done) { $script:kText } else { $script:kWarn })
    $title.AutoSize = $false
    $title.TextAlign = 'MiddleCenter'
    $title.Size = [Drawing.Size]::new($ContentWidth, (U 28))
    $title.Location = [Drawing.Point]::new(0, (U 68))
    $hero.Controls.Add($title)
    $summary = if ($done) {
        if ($script:installResult.Status -eq 'already_current') {
            '客户端已是最新，没有写入文件。'
        } elseif ($null -ne $script:preview) {
            ([string]($script:preview.NewFiles + $script:preview.Replaced + $script:preview.SameFiles)) + ' 个文件已处理：新增 ' + $script:preview.NewFiles + ' · 修改 ' + $script:preview.Replaced + ' · 跳过 ' + $script:preview.SameFiles + "。`r`n现在可以启动游戏，或在维护页检查安装。"
        } else {
            '已写入 ' + $script:installResult.Count + ' 个文件，回读校验通过。'
        }
    } else { $script:installError }
    $detail = New-Label $summary $script:fSmall $script:kText3
    $detail.AutoSize = $false
    $detail.TextAlign = 'TopCenter'
    $detail.Size = [Drawing.Size]::new((U 460), (U 52))
    $detail.Location = [Drawing.Point]::new((U 54), (U 102))
    $hero.Controls.Add($detail)
    $View.Body.Controls.Add($hero)

    $View.Body.Controls.Add((New-SectionTitle '安装结果' ''))
    $info = New-Card ((U 36) * 3 + (U 12)) $script:kSurface2 $script:kLineSoft
    $rows = @(
        @('客户端', $script:clientPath),
        @('版本', $(if ($deploy.Version) { $deploy.Version } else { '—' })),
        @('备份', $(if ($deploy.BackupCount -gt 0) { "host64\dlss5-backup（$($deploy.BackupCount) 个文件）" } elseif ($deploy.Installed) { '无需备份' } else { '—' }))
    )
    $y = 6
    foreach ($pair in $rows) {
        $row = New-KvRow $pair[0] ([string]$pair[1])
        $row.Location = [Drawing.Point]::new((U 0), $y)
        $info.Controls.Add($row)
        $y += $row.Height + (U 1)
    }
    $View.Body.Controls.Add($info)

    $doneButton = New-Button '返回控制台' 'primary' (U 112) (U 32)
    $doneButton.Add_Click({ Show-View 'main' })
    Set-Footer $View '返回维护' { Show-View 'maint' } @(@{ Button = $doneButton })
}
function Update-Maint {
    $view = $maintView
    Clear-Panel $view.Extra
    Clear-Panel $view.Body
    $deploy = Get-DeployInfo

    $view.Body.Controls.Add((New-SectionTitle '已安装' $(if ($deploy.Installed) { '组件正常' } else { '未安装' })))
    $info = New-Card ((U 36) * 4 + (U 12)) $script:kSurface2 $script:kLineSoft
    $rows = @(
        @('客户端', $script:clientPath),
        @('版本', $(if ($deploy.Installed) { $deploy.Version + ' · ' + $deploy.InstalledAt } else { '—' })),
        @('文件', $(if ($deploy.Installed) { "新增 $($deploy.NewFiles) · 修改 $($deploy.Replaced) · 跳过 $($deploy.Same)" } else { '—' })),
        @('备份', $(if ($deploy.BackupCount -gt 0) { "host64\dlss5-backup（$($deploy.BackupCount) 个文件）" } elseif ($deploy.Installed) { '无需备份' } else { '—' }))
    )
    $y = 8
    foreach ($pair in $rows) {
        $row = New-KvRow $pair[0] ([string]$pair[1])
        $row.Location = [Drawing.Point]::new((U 0), $y)
        $info.Controls.Add($row)
        $y += $row.Height + (U 1)
    }
    $view.Body.Controls.Add($info)

    $view.Body.Controls.Add((New-SectionTitle '组件' ''))
    $files = @()
    try { $files = @((Get-Dlss5Status -Client $script:clientPath).Files) } catch { Add-Log ('读取组件状态失败：' + $_.Exception.Message) }
    $tierInfo = Get-TierInfo
    $coreFile = @($files | Where-Object { $_.Name -eq 'd3d9.dll' })
    $srFile = @($files | Where-Object { $_.Name -eq 'host64/nvngx_dlss.dll' })
    $fgFile = @($files | Where-Object { $_.Name -eq 'host64/nvngx_dlssg.dll' })
    $coreOk = ($coreFile.Count -gt 0 -and [string]$coreFile[0].Status -eq '与候选一致')
    $srOk = ($srFile.Count -gt 0 -and [string]$srFile[0].Status -eq '与候选一致')
    $fgOk = ($fgFile.Count -gt 0 -and [string]$fgFile[0].Status -eq '与候选一致')
    $coreVersion = if ($coreFile.Count -gt 0 -and $coreFile[0].Version) { [string]$coreFile[0].Version } elseif ($deploy.Version) { $deploy.Version } else { '' }
    $coreDetail = if ($coreVersion -and $coreVersion -ne '—') { 'v' + $coreVersion } else { '版本未知' }
    $componentCard = New-Card ((U 46) * 3 + (U 6)) $script:kSurface2 $script:kLineSoft
    $componentRows = @(
        @($coreOk, '载体 d3d9.dll', $coreDetail),
        @($srOk, '超分 / 抗锯齿', 'DLSS SR · 模型 ' + $tierInfo.Model),
        @($fgOk, '补帧', 'DLSS FG · MFG 2× / 3× / 4×')
    )
    $y = 4
    foreach ($def in $componentRows) {
        $row = New-CheckRow ([bool]$def[0]) ([string]$def[1]) ([string]$def[2]) $(if ($def[0]) { '正常' } else { '未就绪' })
        $row.Height = (U 46)
        $row.Location = [Drawing.Point]::new((U 0), $y)
        $componentCard.Controls.Add($row)
        $y += $row.Height + (U 1)
    }
    $view.Body.Controls.Add($componentCard)

    $actions = [Windows.Forms.FlowLayoutPanel]::new()
    $actions.Width = $ContentWidth
    $actions.Height = (U 38)
    $actions.WrapContents = $false
    $actions.Margin = [Windows.Forms.Padding]::new((U 0), (U 4), (U 0), (U 6))
    $actions.BackColor = [Drawing.Color]::Transparent
    $refresh = New-Button '检查更新' 'secondary' (U 252) (U 32)
    $refresh.Add_Click({ Invoke-Ui { Refresh-Status; Update-Maint } })
    $repair = New-Button '修复安装' 'secondary' (U 252) (U 32)
    $repair.Add_Click({ Invoke-Ui {
        $ops = @(Get-Dlss5Operations -Client $script:clientPath | Where-Object { $_.Status -eq 'completed' })
        if ($ops.Count -eq 0) { throw '没有可修复的安装事务记录；可改用重新安装。' }
        $result = Invoke-Dlss5Repair -Operation ([string]$ops[$ops.Count - 1].Operation)
        Add-Log ("修复状态：" + $result.Status + "，修复文件 " + $result.Repaired + " 项。")
        Add-Log ('记录：' + $result.Report)
        if (@($result.Errors).Count) { Add-Log (@($result.Errors) -join '；') }
    } })
    $actions.Controls.Add($refresh)
    $actions.Controls.Add($repair)
    $view.Body.Controls.Add($actions)
    $openBackup = New-Button '打开备份目录' 'secondary' $ContentWidth (U 32)
    $openBackup.Margin = [Windows.Forms.Padding]::new((U 0), (U 0), (U 0), (U 6))
    $openBackup.Add_Click({ Invoke-Ui {
        $backup = Join-Path $script:clientPath 'host64/dlss5-backup'
        if (-not [IO.Directory]::Exists($backup)) { throw '当前客户端没有备份目录。' }
        Start-Process -FilePath 'explorer.exe' -ArgumentList @(('"' + $backup + '"'))
        Add-Log ('已请求打开：' + $backup)
    } })
    $view.Body.Controls.Add($openBackup)

    if ($script:showAdvanced) {
        $view.Body.Controls.Add((New-SectionTitle '高级工具' '档位、诊断与运行日志'))
        $schemes = @()
        try { $schemes = @(Get-Dlss5Schemes) } catch { }
        $combo = [Windows.Forms.ComboBox]::new()
        $combo.DropDownStyle = 'DropDownList'
        $combo.Width = (U 200)
        $combo.DisplayMember = 'Name'
        $combo.BackColor = $script:kSurface2
        $combo.ForeColor = $script:kText
        foreach ($scheme in $schemes) { $null = $combo.Items.Add($scheme) }
        if ($combo.Items.Count -gt 0) { $combo.SelectedIndex = 0 }
        $apply = New-Button '应用档位' 'secondary' (U 96) (U 26)
        $apply.Add_Click(({
            if ($combo.SelectedIndex -lt 0) { Add-Log '请先选择档位。'; return }
            Invoke-Ui {
                $picked = $combo.SelectedItem
                $result = Apply-Dlss5Scheme -Client $script:clientPath -Path $picked.Path
                Add-Log ("已应用档位【" + $picked.Name + "】：" + $result.Status + "；重启游戏后生效。")
            }
        }).GetNewClosure())
        $read = New-Button '读取当前值' 'secondary' (U 104) (U 26)
        $read.Add_Click({ Invoke-Ui {
            $advancedValues = Get-Dlss5AdvancedSettings -Client $script:clientPath
            Add-Log ("当前配置：NR=" + $advancedValues.NeuralRendering + "，模型=" + $advancedValues.Model + "，强度=" + $advancedValues.Intensity + "，色调=" + $advancedValues.LocalTone + "，结构=" + $advancedValues.LocalStructure + "，皮肤=" + $advancedValues.SkinStructure)
        } })
        $schemeRow = [Windows.Forms.FlowLayoutPanel]::new()
        $schemeRow.Width = $ContentWidth
        $schemeRow.Height = (U 40)
        $schemeRow.WrapContents = $false
        $schemeRow.BackColor = [Drawing.Color]::Transparent
        $schemeRow.Controls.Add($combo)
        $schemeRow.Controls.Add($apply)
        $schemeRow.Controls.Add($read)
        $view.Body.Controls.Add($schemeRow)

        $saveName = [Windows.Forms.TextBox]::new()
        $saveName.Width = (U 150)
        $saveName.Height = (U 26)
        $saveName.BackColor = $script:kSurface2
        $saveName.ForeColor = $script:kText
        $saveName.BorderStyle = 'FixedSingle'
        $saveScheme = New-Button '保存为方案' 'secondary' (U 104) (U 26)
        $saveScheme.Add_Click(({
            if (-not $saveName.Text.Trim()) { Add-Log '请先填写方案名（1-24 字符）。'; return }
            Invoke-Ui {
                $path = Save-Dlss5Scheme -Client $script:clientPath -Name $saveName.Text
                Add-Log ('方案已保存：' + $path)
            }
        }).GetNewClosure())
        $saveRow = [Windows.Forms.FlowLayoutPanel]::new()
        $saveRow.Width = $ContentWidth
        $saveRow.Height = (U 40)
        $saveRow.WrapContents = $false
        $saveRow.BackColor = [Drawing.Color]::Transparent
        $saveRow.Controls.Add($saveName)
        $saveRow.Controls.Add($saveScheme)
        $view.Body.Controls.Add($saveRow)

        $view.Body.Controls.Add((New-SectionTitle '参数写入' '重启游戏后生效'))

        $nrBox = [Windows.Forms.CheckBox]::new()
        $nrBox.Text = '启用 NR'
        $nrBox.AutoSize = $true
        $nrBox.ForeColor = $script:kText2
        $nrBox.BackColor = $script:kSurface
        $nrBox.FlatStyle = 'Flat'
        $nrBox.Margin = [Windows.Forms.Padding]::new((U 0), (U 7), (U 16), (U 0))
        $modelBox = [Windows.Forms.ComboBox]::new()
        $modelBox.DropDownStyle = 'DropDownList'
        $modelBox.Width = (U 96)
        $modelBox.BackColor = $script:kSurface2
        $modelBox.ForeColor = $script:kText
        foreach ($name in @('模型 1', '模型 2', '模型 3')) { $null = $modelBox.Items.Add($name) }
        $modelBox.SelectedIndex = 0
        $modelBox.Margin = [Windows.Forms.Padding]::new((U 0), (U 2), (U 0), (U 0))
        $modelLabel = New-Label '模型' $script:fSmall $script:kText3
        $modelLabel.Margin = [Windows.Forms.Padding]::new((U 0), (U 8), (U 6), (U 0))
        $paramRow1 = [Windows.Forms.FlowLayoutPanel]::new()
        $paramRow1.Width = $ContentWidth
        $paramRow1.Height = (U 34)
        $paramRow1.WrapContents = $false
        $paramRow1.BackColor = [Drawing.Color]::Transparent
        $paramRow1.Controls.Add($nrBox)
        $paramRow1.Controls.Add($modelLabel)
        $paramRow1.Controls.Add($modelBox)
        $view.Body.Controls.Add($paramRow1)

        function New-NumBox([double]$Min, [double]$Max, [double]$Step, [double]$Value) {
            $box = [Windows.Forms.NumericUpDown]::new()
            $box.Minimum = [decimal]$Min
            $box.Maximum = [decimal]$Max
            $box.Increment = [decimal]$Step
            $box.DecimalPlaces = 2
            $box.Value = [decimal]$Value
            $box.Width = (U 62)
            $box.BackColor = $script:kSurface2
            $box.ForeColor = $script:kText
            $box.BorderStyle = 'FixedSingle'
            $box.Font = $script:fBody
            $box.Margin = [Windows.Forms.Padding]::new((U 0), (U 4), (U 10), (U 0))
            return $box
        }
        $intensityBox = New-NumBox 0 1 0.05 1
        $toneBox = New-NumBox 0 2 0.05 1
        $structureBox = New-NumBox 0 1 0.05 1
        $skinBox = New-NumBox -1 1 0.1 -1
        $paramRow2 = [Windows.Forms.FlowLayoutPanel]::new()
        $paramRow2.Width = $ContentWidth
        $paramRow2.Height = (U 36)
        $paramRow2.WrapContents = $false
        $paramRow2.BackColor = [Drawing.Color]::Transparent
        foreach ($pair in @(@('强度', $intensityBox), @('色调', $toneBox), @('结构', $structureBox), @('皮肤', $skinBox))) {
            $caption = New-Label $pair[0] $script:fSmall $script:kText3
            $caption.Margin = [Windows.Forms.Padding]::new((U 0), (U 8), (U 4), (U 0))
            $paramRow2.Controls.Add($caption)
            $paramRow2.Controls.Add($pair[1])
        }
        $view.Body.Controls.Add($paramRow2)

        $detachedBox = [Windows.Forms.CheckBox]::new()
        $detachedBox.Text = '独立输出候选（需同时启动鼠标助手）'
        $detachedBox.AutoSize = $true
        $detachedBox.ForeColor = $script:kText2
        $detachedBox.BackColor = $script:kSurface
        $detachedBox.FlatStyle = 'Flat'
        $flowBox = [Windows.Forms.CheckBox]::new()
        $flowBox.Text = 'Optical Flow（试验）'
        $flowBox.AutoSize = $true
        $flowBox.ForeColor = $script:kText2
        $flowBox.BackColor = $script:kSurface
        $flowBox.FlatStyle = 'Flat'
        $paramRow3 = [Windows.Forms.FlowLayoutPanel]::new()
        $paramRow3.Width = $ContentWidth
        $paramRow3.Height = (U 32)
        $paramRow3.WrapContents = $false
        $paramRow3.BackColor = [Drawing.Color]::Transparent
        $paramRow3.Controls.Add($detachedBox)
        $paramRow3.Controls.Add($flowBox)
        $view.Body.Controls.Add($paramRow3)

        $readSettings = New-Button '填入当前值' 'secondary' (U 104) (U 30)
        $readSettings.Add_Click(({
            Invoke-Ui {
                $values = Get-Dlss5AdvancedSettings -Client $script:clientPath
                $props = $values.PSObject.Properties
                if ($values.NeuralRendering -in @('0', '1')) { $nrBox.Checked = $values.NeuralRendering -eq '1' }
                if ([string]$values.Model -in @('1', '2', '3')) { $modelBox.SelectedIndex = [int]$values.Model - 1 }
                foreach ($pair in @(@($intensityBox, $values.Intensity, 0, 1), @($toneBox, $values.LocalTone, 0, 2), @($structureBox, $values.LocalStructure, 0, 1), @($skinBox, $values.SkinStructure, -1, 1))) {
                    $parsed = [double]0
                    if ([double]::TryParse([string]$pair[1], [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) {
                        $lo = [double]$pair[2]; $hi = [double]$pair[3]
                        if ($parsed -lt $lo) { $parsed = $lo } elseif ($parsed -gt $hi) { $parsed = $hi }
                        $pair[0].Value = [decimal]$parsed
                    }
                }
                if ($props['DetachedPresentation'] -and $values.DetachedPresentation -in @('0', '1')) { $detachedBox.Checked = $values.DetachedPresentation -eq '1' }
                if ($props['NvidiaOpticalFlowMotion'] -and $values.NvidiaOpticalFlowMotion -in @('0', '1')) { $flowBox.Checked = $values.NvidiaOpticalFlowMotion -eq '1' }
                Add-Log '已从客户端配置读取当前参数。'
            }
        }).GetNewClosure())
        $saveSettings = New-Button '保存设置' 'primary' (U 96) (U 30)
        $saveSettings.Add_Click(({
            Invoke-Ui {
                $result = Set-Dlss5AdvancedSettings -Client $script:clientPath -NeuralRendering $nrBox.Checked `
                    -Model ($modelBox.SelectedIndex + 1) -Intensity ([double]$intensityBox.Value) `
                    -LocalTone ([double]$toneBox.Value) -LocalStructure ([double]$structureBox.Value) -SkinStructure ([double]$skinBox.Value) `
                    -DetachedPresentation $detachedBox.Checked -OpticalFlow $flowBox.Checked
                Add-Log ("设置状态：" + $result.Status + "；备份：" + $result.Backup + "；下次启动游戏生效。")
            }
        }).GetNewClosure())
        $resetSettings = New-Button '恢复候选默认' 'secondary' (U 112) (U 30)
        $resetSettings.Add_Click(({
            Invoke-Ui {
                $result = Set-Dlss5Settings -Client $script:clientPath -NeuralRendering $true -Intensity 1 -Model 1 -DetachedPresentation $true -OpticalFlow $false
                $nrBox.Checked = $true; $modelBox.SelectedIndex = 0; $intensityBox.Value = 1
                $detachedBox.Checked = $true; $flowBox.Checked = $false
                Add-Log ("已恢复候选默认：" + $result.Status + "；重启游戏后生效。")
            }
        }).GetNewClosure())
        $paramRow4 = [Windows.Forms.FlowLayoutPanel]::new()
        $paramRow4.Width = $ContentWidth
        $paramRow4.Height = (U 38)
        $paramRow4.WrapContents = $false
        $paramRow4.BackColor = [Drawing.Color]::Transparent
        $paramRow4.Controls.Add($readSettings)
        $paramRow4.Controls.Add($saveSettings)
        $paramRow4.Controls.Add($resetSettings)
        $view.Body.Controls.Add($paramRow4)
        $tools = [Windows.Forms.FlowLayoutPanel]::new()
        $tools.Width = $ContentWidth
        $tools.Height = (U 80)
        $tools.WrapContents = $true
        $tools.BackColor = [Drawing.Color]::Transparent
        $diagnose = New-Button '导出诊断' 'secondary' (U 96) (U 30)
        $diagnose.Add_Click({ Invoke-Ui {
            $dialog = [Windows.Forms.SaveFileDialog]::new()
            $dialog.Filter = 'JSON 文件 (*.json)|*.json'
            $dialog.FileName = 'dlss5-diagnostic.json'
            $dialog.OverwritePrompt = $false
            try {
                if ($dialog.ShowDialog($form) -eq [Windows.Forms.DialogResult]::OK) {
                    $path = Export-Dlss5Diagnostics -Client $script:clientPath -Destination $dialog.FileName
                    Add-Log ('诊断已保存：' + $path)
                }
            } finally { $dialog.Dispose() }
        } })
        $restore = New-Button '恢复安装前文件' 'secondary' (U 128) (U 30)
        $restore.Add_Click({ Invoke-Ui {
            $ops = @(Get-Dlss5Operations -Client $script:clientPath | Where-Object { $_.Status -eq 'completed' })
            if ($ops.Count -eq 0) { throw '没有可恢复的安装事务记录。' }
            $result = Invoke-Dlss5Restore -Operation ([string]$ops[$ops.Count - 1].Operation)
            Add-Log ("恢复状态：" + $result.Status + "；记录：" + $result.Report)
            Update-Maint
        } })
        $exportList = New-Button '导出移入清单' 'secondary' (U 112) (U 30)
        $exportList.Add_Click({ Invoke-Ui {
            $dialog = [Windows.Forms.SaveFileDialog]::new()
            $dialog.Filter = 'Markdown 文件 (*.md)|*.md'
            $dialog.FileName = 'DLSS5移入清单.md'
            try {
                if ($dialog.ShowDialog($form) -eq [Windows.Forms.DialogResult]::OK) {
                    $path = Export-Dlss5ManifestList -Client $script:clientPath -Destination $dialog.FileName
                    Add-Log ('移入清单已导出：' + $path)
                }
            } finally { $dialog.Dispose() }
        } })
        $openPanel = New-Button '启动游戏内面板' 'secondary' (U 128) (U 30)
        $openPanel.Add_Click({ Invoke-Ui {
            $logDir = Start-PanelProcess
            Add-Log ('游戏内面板已启动。日志：' + $logDir)
        } })
        $watch = New-Button '启动鼠标助手' 'secondary' (U 112) (U 30)
        $watch.Add_Click({ Invoke-Ui {
            $result = Start-Dlss5WindowWatch -Client $script:clientPath -Watcher $script:watcherPath
            $script:lastWatchLog = $result.LogDirectory
            Add-Log ($result.Message + '；日志：' + $result.LogDirectory)
        } })
        $pipeline = New-Button '切换 AIO 管线' 'secondary' (U 112) (U 30)
        $pipeline.Add_Click({ Invoke-Ui {
            $status = Get-Dlss5Status -Client $script:clientPath
            if ($status.Config.PipelineEnabled -notin @('0', '1')) { throw '无法确定当前 AIO 管线开关。' }
            $enable = $status.Config.PipelineEnabled -eq '0'
            $result = Set-Dlss5Pipeline -Client $script:clientPath -Enabled $enable
            Add-Log ("已保存：" + $(if ($enable) { '启用' } else { '停用' }) + " AIO 管线；重启游戏后生效。")
        } })
        foreach ($tool in @($diagnose, $restore, $exportList, $openPanel, $watch, $pipeline)) { $tools.Controls.Add($tool) }
        $view.Body.Controls.Add($tools)

        $maintLogView = New-LogBox (U 110)
        $maintLogView.Panel.Margin = [Windows.Forms.Padding]::new((U 0), (U 4), (U 0), (U 0))
        $view.Body.Controls.Add($maintLogView.Panel)
        $script:logBox = $maintLogView.Box
        Set-LogBoxText $script:logBox $script:logText
    }

    $danger = New-Card (U 120) (C '1E1512') $script:kDanger
    $dangerTitle = New-Label '⚠  移除组件' $script:fBold (C 'E8B4A8')
    $dangerTitle.Location = [Drawing.Point]::new((U 18), (U 14))
    $danger.Controls.Add($dangerTitle)
    $dangerText = New-Label '卸载只处理安装时写入的文件：新增的删除，修改过的从备份还原，跳过的保持原样。用户配置、日志与存档默认保留。' $script:fSmall $script:kText3
    $dangerText.Location = [Drawing.Point]::new((U 18), (U 36))
    $dangerText.MaximumSize = [Drawing.Size]::new(($ContentWidth - (U 36)), (U 44))
    $danger.Controls.Add($dangerText)
    $uninstall = New-Button '查看卸载清单' 'danger' (U 132) (U 32)
    $uninstall.Location = [Drawing.Point]::new((U 18), (U 78))
    $uninstall.Add_Click({ if ((Get-DeployInfo).Installed) { $script:unResult = $null; Show-View 'uninstall' } else { Add-Log '当前客户端没有安装记录，无法卸载。' } })
    $danger.Controls.Add($uninstall)
    $view.Body.Controls.Add($danger)

    $advanced = New-Button $(if ($script:showAdvanced) { '收起高级工具' } else { '高级工具' }) 'ghost' (U 112) (U 30)
    $advanced.BackColor = $script:kSurface
    $advanced.Add_Click({ $script:showAdvanced = -not $script:showAdvanced; Update-Maint })

    Set-Footer $view '返回控制台' { Show-View 'main' } @(@{ Button = $advanced })
}

function Update-Uninstall {
    $view = $uninstallView
    Clear-Panel $view.Extra
    Clear-Panel $view.Body
    $deploy = Get-DeployInfo

    if (-not $deploy.Installed) {
        $view.Body.Controls.Add((New-Label '当前客户端没有安装记录。' $script:fBody $script:kText3))
        Set-Footer $view '返回维护' { Show-View 'maint' } @()
        return
    }

    $entries = @($deploy.Record.Record.entries)
    $remove = @($entries | Where-Object { [string]$_.pre_state -eq 'absent' })
    $restore = @($entries | Where-Object { [string]$_.pre_state -eq 'present' })
    $keep = @($entries | Where-Object { [string]$_.pre_state -notin @('absent', 'present') })
    $modified = @()
    foreach ($entry in $restore) {
        try {
            $path = Join-Path $script:clientPath ([string]$entry.target)
            if ([IO.File]::Exists($path)) {
                $hash = Get-Sha256 $path
                if ($hash -ne [string]$entry.sha256) { $modified += $entry }
            }
        } catch { }
    }

    if ($modified.Count -gt 0) {
        $warnCard = New-Card (U 52) (C '231811') $script:kWarn
        $warnText = New-Label ("有 " + $modified.Count + " 个修改文件已被改动，还原会覆盖当前内容：" + (($modified | ForEach-Object { $_.target }) -join '，')) $script:fSmall $script:kWarn
        $warnText.Location = [Drawing.Point]::new((U 16), (U 16))
        $warnText.MaximumSize = [Drawing.Size]::new(($ContentWidth - (U 32)), (U 40))
        $warnCard.Controls.Add($warnText)
        $view.Body.Controls.Add($warnCard)
    }

    $view.Body.Controls.Add((New-SectionTitle '将删除' ($remove.Count.ToString() + ' 个新增文件')))
    $card = New-Card ([Math]::Max((U 40), $remove.Count * (U 40) + (U 8))) $script:kSurface2 $script:kLineSoft
    $y = 4
    foreach ($entry in $remove) {
        $row = New-FileRow ([string]$entry.target) '删除' $script:kDanger
        $row.Location = [Drawing.Point]::new((U 0), $y)
        $card.Controls.Add($row)
        $y += $row.Height + (U 1)
    }
    if ($remove.Count -eq 0) {
        $none = New-Label '无' $script:fSmall $script:kText3
        $none.Location = [Drawing.Point]::new((U 16), (U 12))
        $card.Controls.Add($none)
    }
    $view.Body.Controls.Add($card)

    $view.Body.Controls.Add((New-SectionTitle '将还原' ($restore.Count.ToString() + ' 个修改文件')))
    $card = New-Card ([Math]::Max((U 40), $restore.Count * (U 40) + (U 8))) $script:kSurface2 $script:kLineSoft
    $y = 4
    foreach ($entry in $restore) {
        $row = New-FileRow ([string]$entry.target) '还原' $script:kAccent
        $row.Location = [Drawing.Point]::new((U 0), $y)
        $card.Controls.Add($row)
        $y += $row.Height + (U 1)
    }
    if ($restore.Count -eq 0) {
        $none = New-Label '无' $script:fSmall $script:kText3
        $none.Location = [Drawing.Point]::new((U 16), (U 12))
        $card.Controls.Add($none)
    }
    $view.Body.Controls.Add($card)

    $residue = @()
    try { $residue = @($deploy.Record.Record.residue | Where-Object { [string]$_.policy -ne 'delete' } | ForEach-Object { [string]$_.path }) } catch { }
    $keepCount = $keep.Count + $residue.Count + 2
    $view.Body.Controls.Add((New-SectionTitle '将保留' ($keepCount.ToString() + ' 项，含用户数据')))
    $card = New-Card ([Math]::Max((U 40), ($keep.Count + $residue.Count + 2) * (U 40) + (U 8))) $script:kSurface2 $script:kLineSoft
    $y = 4
    foreach ($entry in $keep) {
        $row = New-FileRow ([string]$entry.target) '保留' $script:kText3
        $row.Location = [Drawing.Point]::new((U 0), $y)
        $card.Controls.Add($row)
        $y += $row.Height + (U 1)
    }
    foreach ($path in $residue) {
        $row = New-FileRow $path '保留' $script:kText3
        $row.Location = [Drawing.Point]::new((U 0), $y)
        $card.Controls.Add($row)
        $y += $row.Height + (U 1)
    }
    foreach ($label in @('用户配置', '日志与存档')) {
        $row = New-FileRow $label '保留' $script:kText3
        $row.Location = [Drawing.Point]::new((U 0), $y)
        $card.Controls.Add($row)
        $y += $row.Height + (U 1)
    }
    $view.Body.Controls.Add($card)

    $note = New-Label '卸载成功后同时清理安装记录与 host64\dlss5-backup 备份目录；失败不掩盖，备份缺失时不删当前文件。' $script:fSmall $script:kText3 (U 520)
    $note.Margin = [Windows.Forms.Padding]::new((U 0), (U 6), (U 0), (U 0))
    $view.Body.Controls.Add($note)

    if ($null -ne $script:unResult) {
        $view.Body.Controls.Add((New-SectionTitle '卸载结果' ''))
        $result = $script:unResult
        $card = New-Card (U 148) $script:kSurface2 $(if (@($result.Errors).Count) { $script:kWarn } else { $script:kLineSoft })
        $rows = @(
            @('状态', [string]$result.Status),
            @('已还原', ([string]$result.Restored + ' 项')),
            @('已删除', ([string]$result.Deleted + ' 项')),
            @('保留改动', ([string]$result.SavedModified + ' 项')),
            @('清理产物', ([string]$result.ResidueRemoved + ' 项'))
        )
        $y = 6
        foreach ($pair in $rows) {
            $row = New-KvRow $pair[0] $pair[1]
            $row.Location = [Drawing.Point]::new((U 0), $y)
            $card.Controls.Add($row)
            $y += $row.Height + (U 1)
        }
        $view.Body.Controls.Add($card)
        if (@($result.Errors).Count) {
            $errorText = New-Label (@($result.Errors) -join '；') $script:fSmall $script:kWarn (U 520)
            $view.Body.Controls.Add($errorText)
        }
    }

    $run = New-Button '确认卸载' 'danger' (U 112) (U 32)
    if ($null -ne $script:unResult) { $run.Enabled = $false; $run.BackColor = $script:kSurface3; $run.ForeColor = $script:kText3 }
    $run.Add_Click({ Invoke-Ui {
        $script:unResult = Invoke-Dlss5Uninstall -Client $script:clientPath
        Add-Log ("卸载状态：" + $script:unResult.Status + "；还原 " + $script:unResult.Restored + " 项，删除 " + $script:unResult.Deleted + " 项。")
        Update-Uninstall
    } })
    Set-Footer $view '' $null @(@{ Button = $run })
}

function Invoke-Ui([scriptblock]$Action) {
    foreach ($k in @('main', 'setup', 'maint', 'uninstall')) { $script:viewDirty[$k] = $true }
    $form.UseWaitCursor = $true
    [Windows.Forms.Application]::DoEvents()
    try { & $Action }
    catch { Add-Log ('操作未完成：' + $_.Exception.Message) }
    finally { $form.UseWaitCursor = $false }
}

$form.KeyPreview = $true
$form.Add_KeyDown({
    param($sender, $eventArgs)
    if ($eventArgs.KeyCode -eq [Windows.Forms.Keys]::Escape) {
        if ($script:view -eq 'uninstall') { Show-View 'maint' }
        elseif ($script:view -in @('setup', 'maint')) { Show-View 'main' }
        elseif ($script:view -eq 'main') { $form.Close() }
    }
})

$watchTimer = [Windows.Forms.Timer]::new()
$watchTimer.Interval = 10000
$watchTimer.Add_Tick({
    try {
        # zh: 用 Get-Process 代替 Win32_Process CIM 查询：CIM 每次约 0.3-0.7 秒，
        # 定时器在 UI 线程上执行时会顶住界面（20261113 卡顿排查）。
        if (-not (Test-GameRunning)) { return }
        if (@(Get-Process -Name 'dlss5-panel' -ErrorAction SilentlyContinue).Count -gt 0) { return }
        $null = Start-PanelProcess
        Add-Log '检测到游戏运行但面板未启动，已自动拉起。'
    } catch {
        Add-Log ('面板自动拉起未执行：' + $_.Exception.Message)
    }
})
$watchTimer.Start()
$uiTimer = [Windows.Forms.Timer]::new()
$uiTimer.Interval = 1000
$uiTimer.Add_Tick({ Update-RuntimeState })
$uiTimer.Start()
$form.Add_FormClosed({ $uiTimer.Stop(); $watchTimer.Stop() })
Show-View 'main'
if ($SmokeView) {
    switch ($SmokeView) {
        's1' { Show-View 'setup'; Go-SetupStep 1 }
        's2' { Show-View 'setup'; Go-SetupStep 2 }
        's3' { Show-View 'setup'; Go-SetupStep 3 }
        's4' { Show-View 'setup'; Go-SetupStep 4 }
        's5' { Show-View 'setup'; Go-SetupStep 5 }
        'maint' { Show-View 'maint' }
        'uninstall' { Show-View 'uninstall' }
        default { Show-View 'main' }
    }
}
if ($AutoCloseSeconds -gt 0) {
    $autoTimer = [Windows.Forms.Timer]::new()
    $autoTimer.Interval = $AutoCloseSeconds * 1000
    $autoTimer.Add_Tick({ $autoTimer.Stop(); $form.Close() })
    $autoTimer.Start()
}
if ($env:DLSS_UI_DUMP) {
    function Get-DeepControls($Root) {
        foreach ($child in $Root.Controls) { $child; Get-DeepControls $child }
    }
    $dumpTimer = [Windows.Forms.Timer]::new()
    $dumpTimer.Interval = 3000
    $dumpTimer.Add_Tick({
        $dumpTimer.Stop()
        $lines = @()
        foreach ($c in @(Get-DeepControls $form)) {
            if (-not ($c -is [Windows.Forms.Label]) -or -not $c.Text) { continue }
            $need = 0
            try { $need = [Windows.Forms.TextRenderer]::MeasureText($c.Text, $c.Font).Width } catch { }
            $avail = $c.ClientSize.Width
            $flag = if (-not $c.AutoSize -and $need -gt $avail -and -not $c.AutoEllipsis) { 'CLIP' } else { 'ok' }
            $lines += ($flag + ' | ' + $c.Text + ' | w=' + $avail + ' need=' + $need + ' ellipsis=' + $c.AutoEllipsis + ' autosize=' + $c.AutoSize)
        }
        [IO.File]::WriteAllLines(($env:TEMP + '\opencode\labels.txt'), $lines, [Text.UTF8Encoding]::new($false))
    })
    $dumpTimer.Start()
}
if ($Smoke) {
    $form.Dispose()
    Write-Output 'smoke ok'
    return
}
$null = $form.ShowDialog()
$form.Dispose()
