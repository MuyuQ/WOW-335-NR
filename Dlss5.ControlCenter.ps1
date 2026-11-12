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
Import-Module (Join-Path $PSScriptRoot 'Dlss5.Core.psm1') -Force
[Windows.Forms.Application]::EnableVisualStyles()

$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$batch = Join-Path $project '_scratch/dlss5-aio/20261106-093536'
if (-not $Client) { $Client = Join-Path $batch 'client' }
if (-not $Stage) { $Stage = Join-Path $batch 'candidate/staged/control-center-candidate-20261106' }
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

$script:fBody = [Drawing.Font]::new('Microsoft YaHei UI', 9)
$script:fSmall = [Drawing.Font]::new('Microsoft YaHei UI', 8.25)
$script:fBold = [Drawing.Font]::new('Microsoft YaHei UI', 9, [Drawing.FontStyle]::Bold)
$script:fTitle = [Drawing.Font]::new('Microsoft YaHei UI', 14, [Drawing.FontStyle]::Bold)
$script:fHero = [Drawing.Font]::new('Constantia', 19)
$script:fMono = [Drawing.Font]::new('Consolas', 9)

$script:clientPath = $Client
$script:stagePath = $Stage
$script:watcherPath = $Watcher
$script:view = 'main'
$script:wizStep = 1
$script:preview = $null
$script:status = $null
$script:checks = @()
$script:installResult = $null
$script:installError = $null
$script:lastPanelLog = $null
$script:lastWatchLog = $null
$script:logText = "请选择客户端目录，点击「检查状态」。`r`n安装只写入发行包清单内的文件，替换前会先备份到客户端内的 host64\dlss5-backup。"
$script:logBox = $null
$script:mainStatus = $null
$script:showAdvanced = $false
$script:maintNote = ''
$script:unResult = $null
$script:wizBusy = $false

$ContentWidth = 520

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
}

function New-Label([string]$Text, $Font = $null, $Color = $null, [int]$Width = 0) {
    $label = [Windows.Forms.Label]::new()
    $label.Text = $Text
    $label.AutoSize = $true
    $label.BackColor = [Drawing.Color]::Transparent
    $label.Font = if ($Font) { $Font } else { $script:fBody }
    $label.ForeColor = if ($null -ne $Color) { $Color } else { $script:kText2 }
    $label.Margin = [Windows.Forms.Padding]::new(0, 0, 0, 0)
    if ($Width -gt 0) { $label.MaximumSize = [Drawing.Size]::new($Width, 0) }
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
    $button.Margin = [Windows.Forms.Padding]::new(0, 0, 8, 0)
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

function New-Card([int]$Height, $Fill = $null, $Border = $null, [int]$Width = 0) {
    $card = [Windows.Forms.Panel]::new()
    $card.Width = if ($Width -gt 0) { $Width } else { $ContentWidth }
    $card.Height = $Height
    $card.BackColor = if ($Fill) { $Fill } else { $script:kSurface2 }
    $card.Margin = [Windows.Forms.Padding]::new(0, 0, 0, 12)
    Set-Rounded $card 9
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
    $row.Height = 30
    $row.BackColor = [Drawing.Color]::Transparent
    $row.Margin = [Windows.Forms.Padding]::new(0, 6, 0, 6)
    $mark = New-Label '◆' $script:fSmall $script:kAccent
    $mark.Location = [Drawing.Point]::new(0, 6)
    $row.Controls.Add($mark)
    $title = New-Label $Text $script:fBold $script:kText
    $title.Location = [Drawing.Point]::new(16, 4)
    $row.Controls.Add($title)
    if ($Note) {
        $noteLabel = New-Label $Note $script:fSmall $script:kText3
        $noteLabel.Location = [Drawing.Point]::new(16 + $title.PreferredWidth + 10, 6)
        $row.Controls.Add($noteLabel)
    }
    return $row
}

function New-KvRow([string]$Key, [string]$Value, $ValueColor = $null, [int]$Height = 30) {
    $row = [Windows.Forms.Panel]::new()
    $row.Width = $ContentWidth
    $row.Height = $Height
    $row.BackColor = [Drawing.Color]::Transparent
    $row.Margin = [Windows.Forms.Padding]::new(0, 0, 0, 0)
    $keyLabel = New-Label $Key $script:fSmall $script:kText3
    $keyLabel.Location = [Drawing.Point]::new(16, 6)
    $keyLabel.Width = 54
    $row.Controls.Add($keyLabel)
    $valueLabel = New-Label $Value $script:fBody $(if ($null -ne $ValueColor) { $ValueColor } else { $script:kText2 })
    $valueLabel.Location = [Drawing.Point]::new(76, 3)
    $valueLabel.MaximumSize = [Drawing.Size]::new(($ContentWidth - 96), 0)
    $row.Controls.Add($valueLabel)
    return $row
}

function New-FileRow([string]$Name, [string]$Tag, $TagColor, [string]$Note = '') {
    $row = [Windows.Forms.Panel]::new()
    $row.Width = $ContentWidth
    $row.Height = 34
    $row.BackColor = [Drawing.Color]::Transparent
    $row.Margin = [Windows.Forms.Padding]::new(0, 0, 0, 1)
    $nameLabel = New-Label $Name $script:fMono $script:kText
    $nameLabel.Location = [Drawing.Point]::new(16, 8)
    $row.Controls.Add($nameLabel)
    $tagLabel = New-Label $Tag $script:fSmall $TagColor
    $tagLabel.Location = [Drawing.Point]::new(($ContentWidth - 92), 9)
    $tagLabel.Width = 76
    $tagLabel.TextAlign = 'MiddleRight'
    $row.Controls.Add($tagLabel)
    if ($Note) {
        $noteLabel = New-Label $Note $script:fSmall $script:kText3
        $noteLabel.Location = [Drawing.Point]::new(16 + $nameLabel.PreferredWidth + 10, 9)
        $row.Controls.Add($noteLabel)
    }
    return $row
}

function New-CheckRow([bool]$Ok, [string]$Text, [string]$Detail = '') {
    $row = [Windows.Forms.Panel]::new()
    $row.Width = $ContentWidth
    $row.Height = 38
    $row.BackColor = [Drawing.Color]::Transparent
    $row.Margin = [Windows.Forms.Padding]::new(0, 0, 0, 1)
    $icon = New-Label $(if ($Ok) { '✓' } else { '!' }) $script:fBold $(if ($Ok) { $script:kJade } else { $script:kWarn })
    $icon.Location = [Drawing.Point]::new(16, 9)
    $row.Controls.Add($icon)
    $main = New-Label $Text $script:fBody $script:kText
    $main.Location = [Drawing.Point]::new(36, 3)
    $row.Controls.Add($main)
    if ($Detail) {
        $detailLabel = New-Label $Detail $script:fSmall $(if ($Ok) { $script:kText3 } else { $script:kWarn })
        $detailLabel.Location = [Drawing.Point]::new(36, 20)
        $detailLabel.MaximumSize = [Drawing.Size]::new(($ContentWidth - 56), 0)
        $row.Controls.Add($detailLabel)
    }
    return $row
}

function Add-Log([string]$Text) {
    $stamp = Get-Date -Format 'HH:mm:ss'
    $script:logText = $script:logText + "`r`n[$stamp] " + $Text
    if ($script:logBox -and -not $script:logBox.IsDisposed) { $script:logBox.Text = $script:logText }
}

$form = [Windows.Forms.Form]::new()
$form.Text = 'WOW-335-NR 控制台'
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'None'
$form.ClientSize = [Drawing.Size]::new(600, 900)
$form.BackColor = $script:kSurface
$form.Font = $script:fBody
$form.KeyPreview = $true

$titleBar = [Windows.Forms.Panel]::new()
$titleBar.Dock = 'Top'
$titleBar.Height = 44
$titleBar.BackColor = $script:kChrome
$titleBar.Tag = $script:kLine
$titleBar.Add_Paint({
    param($sender, $eventArgs)
    $pen = [Drawing.Pen]::new([Drawing.Color]$sender.Tag, 1)
    $eventArgs.Graphics.DrawLine($pen, 0, $sender.Height - 1, $sender.Width, $sender.Height - 1)
    $pen.Dispose()
})
$brand = New-Label '◆' ([Drawing.Font]::new('Microsoft YaHei UI', 10)) $script:kAccent
$brand.Location = [Drawing.Point]::new(18, 13)
$titleBar.Controls.Add($brand)
$titleText = New-Label 'WOW-335-NR  控制台' $script:fBold $script:kText
$titleText.Location = [Drawing.Point]::new(38, 12)
$titleBar.Controls.Add($titleText)
$script:dragStart = $null
$script:dragOrigin = [Drawing.Point]::new(0, 0)
foreach ($dragTarget in @($titleBar, $titleText, $brand)) {
    $dragTarget.Add_MouseDown({ param($sender, $e) if ($e.Button -eq [Windows.Forms.MouseButtons]::Left) { $script:dragStart = [Windows.Forms.Cursor]::Position; $script:dragOrigin = $form.Location } })
    $dragTarget.Add_MouseMove({ param($sender, $e) if ($null -ne $script:dragStart -and $e.Button -eq [Windows.Forms.MouseButtons]::Left) { $now = [Windows.Forms.Cursor]::Position; $form.Location = [Drawing.Point]::new(($script:dragOrigin.X + $now.X - $script:dragStart.X), ($script:dragOrigin.Y + $now.Y - $script:dragStart.Y)) } })
    $dragTarget.Add_MouseUp({ param($sender, $e) $script:dragStart = $null })
}
$titleText.Add_DoubleClick({ if ($form.WindowState -eq 'Maximized') { $form.WindowState = 'Normal' } else { $form.WindowState = 'Maximized' } })
$titleBar.Add_DoubleClick({ if ($form.WindowState -eq 'Maximized') { $form.WindowState = 'Normal' } else { $form.WindowState = 'Maximized' } })
$winButtons = [Windows.Forms.FlowLayoutPanel]::new()
$winButtons.Dock = 'Right'
$winButtons.Width = 108
$winButtons.Height = 44
$winButtons.BackColor = [Drawing.Color]::Transparent
$winButtons.FlowDirection = 'LeftToRight'
$winButtons.WrapContents = $false
$winButtons.Padding = [Windows.Forms.Padding]::new(0, 7, 0, 0)
$minimize = New-Button '—' 'ghost' 34 30
$maximize = New-Button '□' 'ghost' 34 30
$close = New-Button '✕' 'ghost' 34 30
$minimize.Font = [Drawing.Font]::new('Microsoft YaHei UI', 11)
$maximize.Font = [Drawing.Font]::new('Microsoft YaHei UI', 9)
$close.Font = [Drawing.Font]::new('Microsoft YaHei UI', 11)
$winButtons.Controls.Add($minimize)
$winButtons.Controls.Add($maximize)
$winButtons.Controls.Add($close)
$titleBar.Controls.Add($winButtons)
$minimize.Add_Click({ $form.WindowState = 'Minimized' })
$maximize.Add_Click({ if ($form.WindowState -eq 'Maximized') { $form.WindowState = 'Normal' } else { $form.WindowState = 'Maximized' } })
$close.Add_Click({ $form.Close() })
$form.Controls.Add($titleBar)

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
    $body.Padding = [Windows.Forms.Padding]::new(26, 18, 14, 20)

    $header = [Windows.Forms.Panel]::new()
    $header.Dock = 'Top'
    $header.Height = 116
    $header.BackColor = $script:kSurface

    $back = New-Button ('← ' + $BackText) 'ghost' 84 26
    $back.Font = $script:fSmall
    $back.BackColor = $script:kSurface
    $back.Location = [Drawing.Point]::new(20, 12)
    $back.Add_Click($BackAction)
    $header.Controls.Add($back)

    $headingLabel = New-Label $Title $script:fTitle $script:kText
    $headingLabel.Location = [Drawing.Point]::new(26, 44)
    $header.Controls.Add($headingLabel)

    $sub = New-Label $Subtitle $script:fSmall $script:kText3
    $sub.Location = [Drawing.Point]::new(26, 76)
    $sub.MaximumSize = [Drawing.Size]::new(548, 0)
    $header.Controls.Add($sub)

    $extra = [Windows.Forms.Panel]::new()
    $extra.Location = [Drawing.Point]::new(26, 98)
    $extra.Width = 548
    $extra.Height = 16
    $extra.BackColor = [Drawing.Color]::Transparent
    $header.Controls.Add($extra)

    $footer = [Windows.Forms.Panel]::new()
    $footer.Dock = 'Bottom'
    $footer.Height = 64
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
    $form.Controls.Add($panel)
    return [pscustomobject]@{ Panel = $panel; Body = $body; Header = $header; Extra = $extra; Footer = $footer; Back = $back }
}

$mainView = New-ViewShell '控制台' '选择客户端后安装、维护或查看运行状态。' '退出' { $form.Close() }
$setupView = New-ViewShell '安装到客户端' '只向客户端写入发行包内的文件。一致的文件跳过，不同的文件先备份再替换。' '返回控制台' { Show-View 'main' }
$maintView = New-ViewShell '维护' '查看已安装组件与备份位置，或移除本工具写入客户端的文件。' '返回控制台' { Show-View 'main' }
$uninstallView = New-ViewShell '卸载组件' '逐项核对将在客户端中处理的文件，确认后执行。' '返回维护' { Show-View 'maint' }
$mainView.Back.Visible = $false
$mainView.Header.Height = 84
$views = @{ main = $mainView; setup = $setupView; maint = $maintView; uninstall = $uninstallView }

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
        $left = New-Button $LeftText 'ghost' 110 30
        $left.BackColor = $script:kSurface
        $left.Location = [Drawing.Point]::new(12, 17)
        if ($LeftAction) { $left.Add_Click($LeftAction) }
        $View.Footer.Controls.Add($left)
    }
    $flow = [Windows.Forms.FlowLayoutPanel]::new()
    $flow.FlowDirection = 'LeftToRight'
    $flow.WrapContents = $false
    $flow.AutoSize = $true
    $flow.Dock = 'Right'
    $flow.BackColor = [Drawing.Color]::Transparent
    $flow.Padding = [Windows.Forms.Padding]::new(0, 16, 18, 0)
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

function Show-View([string]$Name) {
    $script:view = $Name
    foreach ($key in $views.Keys) { $views[$key].Panel.Visible = ($key -eq $Name) }
    switch ($Name) {
        'main' { Update-Main }
        'setup' { Update-Setup }
        'maint' { Update-Maint }
        'uninstall' { Update-Uninstall }
    }
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
    $deploy = Get-DeployInfo
    $script:logBox = $null

    $header = New-SectionTitle '本地部署' ''
    $view.Body.Controls.Add($header)

    $hero = New-Card 126 $script:kSurface2 $script:kLine
    $hero.Controls.Add((New-Label '安装状态' $script:fSmall $script:kText3))
    $hero.Controls[0].Location = [Drawing.Point]::new(18, 16)
    $stateText = if ($deploy.Installed) { '已部署 ' + $deploy.Version } else { '未安装' }
    $stateLabel = New-Label $stateText $script:fHero $(if ($deploy.Installed) { $script:kJadeText } else { $script:kText2 })
    $stateLabel.Location = [Drawing.Point]::new(18, 38)
    $hero.Controls.Add($stateLabel)
    $when = if ($deploy.Installed) { '安装于 ' + $deploy.InstalledAt } else { '选择客户端后开始安装' }
    $whenLabel = New-Label $when $script:fSmall $script:kText3
    $whenLabel.Location = [Drawing.Point]::new(18, 84)
    $hero.Controls.Add($whenLabel)
    $clientLabel = New-Label $script:clientPath $script:fSmall $script:kText2
    $clientLabel.AutoSize = $false
    $clientLabel.AutoEllipsis = $true
    $clientLabel.Width = 300
    $clientLabel.Height = 18
    $clientLabel.Location = [Drawing.Point]::new(18, 104)
    $hero.Controls.Add($clientLabel)

    $install = New-Button $(if ($deploy.Installed) { '重新安装' } else { '安装到客户端' }) 'primary' 112 34
    $install.Location = [Drawing.Point]::new(386, 20)
    $install.Add_Click({ if (-not $script:wizBusy) { Open-Wizard } })
    $hero.Controls.Add($install)
    $maint = New-Button '维护' 'secondary' 112 30
    $maint.Location = [Drawing.Point]::new(386, 62)
    $maint.Add_Click({ if ((Get-DeployInfo).Installed) { Show-View 'maint' } else { Add-Log '当前客户端还没有安装记录。' } })
    $hero.Controls.Add($maint)
    $view.Body.Controls.Add($hero)

    $view.Body.Controls.Add((New-SectionTitle '部署详情' ''))
    $deployCard = New-Card 102 $script:kSurface $script:kLineSoft
    $versionRow = New-KvRow '部署' $(if ($deploy.Installed) { "已部署 $($deploy.Version) · $($deploy.InstalledAt)" } else { '未部署' }) $(if ($deploy.Installed) { $script:kJadeText } else { $script:kText3 })
    $versionRow.Location = [Drawing.Point]::new(0, 8)
    $deployCard.Controls.Add($versionRow)
    $dupText = if (-not $deploy.Installed) { '—' }
        elseif ($deploy.Replaced -gt 0) { "$($deploy.Replaced) 项已备份并替换 · $($deploy.Same) 项一致跳过" }
        else { "无重复文件 · $($deploy.Same) 项一致跳过" }
    $dupRow = New-KvRow '重复' $dupText $(if ($deploy.Installed) { $script:kText2 } else { $script:kText3 })
    $dupRow.Location = [Drawing.Point]::new(0, 38)
    $deployCard.Controls.Add($dupRow)
    $backupText = if (-not $deploy.Installed) { '—' }
        elseif ($deploy.BackupCount -gt 0) { "$($deploy.BackupCount) 个文件 · host64\dlss5-backup" }
        else { '无需备份' }
    $backupRow = New-KvRow '备份' $backupText $(if ($deploy.BackupCount -gt 0) { $script:kText2 } else { $script:kText3 })
    $backupRow.Location = [Drawing.Point]::new(0, 68)
    $deployCard.Controls.Add($backupRow)
    $view.Body.Controls.Add($deployCard)

    $view.Body.Controls.Add((New-SectionTitle '运行状态' '只读检查'))
    $statusCard = New-Card 96 $script:kSurface $script:kLineSoft
    $gameText = '未检查'
    $hostText = '未检查'
    $recordText = '未检查'
    if ($null -ne $script:status) {
        $gameText = if ($script:status.GameProcesses -gt 0) { "运行中（$($script:status.GameProcesses)）" } else { '未运行' }
        $hostText = if ($script:status.HostProcesses -gt 0) { "运行中（$($script:status.HostProcesses)）" } else { '未运行' }
        $recordText = [string]$script:status.InstallRecord
    }
    $gameRow = New-KvRow '游戏' $gameText
    $gameRow.Location = [Drawing.Point]::new(0, 8)
    $statusCard.Controls.Add($gameRow)
    $hostRow = New-KvRow '载体' $hostText
    $hostRow.Location = [Drawing.Point]::new(0, 38)
    $statusCard.Controls.Add($hostRow)
    $recordRow = New-KvRow '记录' $recordText
    $recordRow.Location = [Drawing.Point]::new(0, 68)
    $statusCard.Controls.Add($recordRow)
    $script:mainStatus = @{
        Game = $gameRow.Controls[1]
        Host = $hostRow.Controls[1]
        Record = $recordRow.Controls[1]
    }
    $view.Body.Controls.Add($statusCard)
    $view.Body.Controls.Add((New-SectionTitle '日志' '最近输出'))
    $logBox = [Windows.Forms.TextBox]::new()
    $logBox.Multiline = $true
    $logBox.ReadOnly = $true
    $logBox.ScrollBars = 'Vertical'
    $logBox.WordWrap = $true
    $logBox.Width = $ContentWidth
    $logBox.Height = 150
    $logBox.BackColor = $script:kSurface
    $logBox.ForeColor = $script:kText2
    $logBox.BorderStyle = 'FixedSingle'
    $logBox.Font = $script:fMono
    $logBox.Text = $script:logText
    $logBox.Margin = [Windows.Forms.Padding]::new(0, 0, 0, 10)
    $script:logBox = $logBox
    $view.Body.Controls.Add($logBox)

    $actions = [Windows.Forms.FlowLayoutPanel]::new()
    $actions.Width = $ContentWidth
    $actions.Height = 36
    $actions.WrapContents = $false
    $actions.BackColor = [Drawing.Color]::Transparent
    $actions.Margin = [Windows.Forms.Padding]::new(0, 0, 0, 0)
    $check = New-Button '检查状态' 'secondary' 96 32
    $check.Add_Click({ Invoke-Ui { Refresh-Status; Add-Log '已刷新客户端状态。' } })
    $panelLog = New-Button '打开面板日志' 'secondary' 110 32
    $panelLog.Add_Click({ Invoke-Ui {
        if (-not $script:lastPanelLog -or -not [IO.Directory]::Exists($script:lastPanelLog)) { throw '本次窗口尚未启动面板。' }
        $lines = @()
        foreach ($name in @('stdout.log', 'stderr.log')) {
            $path = Join-Path $script:lastPanelLog $name
            if ([IO.File]::Exists($path)) { $lines += @(Get-Content -LiteralPath $path -Tail 40 -Encoding UTF8 -ErrorAction Stop) }
        }
        Add-Log $(if ($lines.Count) { $lines -join "`r`n" } else { '面板暂无日志。' })
    } })
    $watchLog = New-Button '打开助手日志' 'secondary' 110 32
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

    Set-Footer $view '' $null @(
        @{ Button = (New-Button '切换客户端' 'ghost' 96 30); Action = { Open-Wizard } },
        @{ Button = (New-Button '维护' 'ghost' 64 30); Action = { if ((Get-DeployInfo).Installed) { Show-View 'maint' } else { Add-Log '当前客户端还没有安装记录。' } } }
    )
    $pathLabel = New-Label ('客户端：' + $script:clientPath) $script:fSmall $script:kText3
    $pathLabel.AutoSize = $false
    $pathLabel.AutoEllipsis = $true
    $pathLabel.Width = 300
    $pathLabel.Height = 18
    $pathLabel.Location = [Drawing.Point]::new(20, 24)
    $view.Footer.Controls.Add($pathLabel)
    $pathLabel.BringToFront()
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
            $script:mainStatus.Record.Text = [string]$script:status.InstallRecord
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
    Show-View 'setup'
}

function Go-SetupStep([int]$Step) {
    $script:wizStep = $Step
    Update-Setup
}

function Update-Setup {
    $view = $setupView
    Clear-Panel $view.Extra
    Clear-Panel $view.Body

    $stepNames = @('选择客户端', '环境检查', '文件清单', '安装', '完成')
    $steps = [Windows.Forms.FlowLayoutPanel]::new()
    $steps.Width = 548
    $steps.Height = 16
    $steps.WrapContents = $false
    $steps.BackColor = [Drawing.Color]::Transparent
    for ($i = 0; $i -lt $stepNames.Count; $i++) {
        $n = $i + 1
        $color = if ($n -eq $script:wizStep) { $script:kAccent } elseif ($n -lt $script:wizStep) { $script:kText2 } else { $script:kText3 }
        $label = New-Label ("$n " + $stepNames[$i]) $script:fSmall $color
        $steps.Controls.Add($label)
        if ($n -lt $stepNames.Count) {
            $line = New-Label '·' $script:fSmall $script:kText3
            $line.Margin = [Windows.Forms.Padding]::new(6, 0, 6, 0)
            $steps.Controls.Add($line)
        }
    }
    $view.Extra.Controls.Add($steps)

    switch ($script:wizStep) {
        1 { Build-SetupStep1 $view }
        2 { Build-SetupStep2 $view }
        3 { Build-SetupStep3 $view }
        4 { Build-SetupStep4 $view }
        5 { Build-SetupStep5 $view }
    }
}

function Build-SetupStep1($View) {
    $View.Body.Controls.Add((New-SectionTitle '客户端目录' '选择含 wow.exe 的根目录'))
    $clientCard = New-Card 58 $script:kSurface $script:kLineSoft
    $box = [Windows.Forms.TextBox]::new()
    $box.Text = $script:clientPath
    $box.ReadOnly = $true
    $box.Width = 388
    $box.Height = 26
    $box.Location = [Drawing.Point]::new(16, 16)
    $box.BackColor = $script:kSurface2
    $box.ForeColor = $script:kText
    $box.BorderStyle = 'FixedSingle'
    $box.Font = $script:fBody
    $clientCard.Controls.Add($box)
    $browse = New-Button '浏览' 'secondary' 84 26
    $browse.Location = [Drawing.Point]::new(418, 16)
    $browse.Add_Click({
        $dialog = [Windows.Forms.FolderBrowserDialog]::new()
        try {
            if ([IO.Directory]::Exists($script:clientPath)) { $dialog.SelectedPath = $script:clientPath }
            if ($dialog.ShowDialog($form) -eq [Windows.Forms.DialogResult]::OK) {
                $script:clientPath = $dialog.SelectedPath
                $script:wizStep = 1
                Update-Setup
            }
        } finally { $dialog.Dispose() }
    })
    $clientCard.Controls.Add($browse)
    $View.Body.Controls.Add($clientCard)

    $clientState = Test-ClientDir $script:clientPath
    $View.Body.Controls.Add((New-CheckRow ($clientState -eq '已识别客户端') '客户端目录' $clientState))

    $View.Body.Controls.Add((New-SectionTitle '组件来源' '发行包或暂存目录，含清单文件'))
    $stageCard = New-Card 58 $script:kSurface $script:kLineSoft
    $stageBox = [Windows.Forms.TextBox]::new()
    $stageBox.Text = $script:stagePath
    $stageBox.ReadOnly = $true
    $stageBox.Width = 388
    $stageBox.Height = 26
    $stageBox.Location = [Drawing.Point]::new(16, 16)
    $stageBox.BackColor = $script:kSurface2
    $stageBox.ForeColor = $script:kText
    $stageBox.BorderStyle = 'FixedSingle'
    $stageBox.Font = $script:fBody
    $stageCard.Controls.Add($stageBox)
    $stageBrowse = New-Button '浏览' 'secondary' 84 26
    $stageBrowse.Location = [Drawing.Point]::new(418, 16)
    $stageBrowse.Add_Click({
        $dialog = [Windows.Forms.FolderBrowserDialog]::new()
        try {
            if ([IO.Directory]::Exists($script:stagePath)) { $dialog.SelectedPath = $script:stagePath }
            if ($dialog.ShowDialog($form) -eq [Windows.Forms.DialogResult]::OK) {
                $script:stagePath = $dialog.SelectedPath
                $script:wizStep = 1
                Update-Setup
            }
        } finally { $dialog.Dispose() }
    })
    $stageCard.Controls.Add($stageBrowse)
    $View.Body.Controls.Add($stageCard)
    $stageState = if (-not $script:stagePath) { '未选择组件来源' } elseif ([IO.Directory]::Exists($script:stagePath)) { '来源目录存在' } else { '来源目录不存在' }
    $View.Body.Controls.Add((New-CheckRow ($stageState -eq '来源目录存在') '组件来源' $stageState))

    $hint = New-Label '控制台可独立于客户端安装；安装完成后仍可从维护页修复或卸载。' $script:fSmall $script:kText3 520
    $hint.Margin = [Windows.Forms.Padding]::new(0, 10, 0, 0)
    $View.Body.Controls.Add($hint)

    Set-Footer $View '返回控制台' { Show-View 'main' } @(
        @{ Button = (New-Button '开始检查' 'primary' 112 32); Action = { Start-Precheck } }
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
    $card = New-Card ($(if ($script:checks.Count -gt 0) { $script:checks.Count * 40 + 8 } else { 52 })) $script:kSurface $script:kLineSoft
    $y = 4
    foreach ($check in @($script:checks)) {
        $row = New-CheckRow ([bool]$check.Ok) ([string]$check.Text) ([string]$check.Detail)
        $row.Location = [Drawing.Point]::new(0, $y)
        $card.Controls.Add($row)
        $y += 40
    }
    if ($script:checks.Count -eq 0) {
        $empty = New-Label '预检没有完成，请返回上一步重试。' $script:fBody $script:kWarn
        $empty.Location = [Drawing.Point]::new(16, 16)
        $card.Controls.Add($empty)
    }
    $View.Body.Controls.Add($card)

    if ($null -ne $script:preview) {
        $summary = New-Label ("可安装：" + $(if ($script:preview.CanInstall) { '是' } else { '否，请先处理冲突' }) + "　新增 " + $script:preview.NewFiles + " · 替换 " + $script:preview.Replaced + " · 跳过 " + $script:preview.SameFiles) $script:fSmall $(if ($script:preview.CanInstall) { $script:kText2 } else { $script:kWarn }) 520
        $summary.Margin = [Windows.Forms.Padding]::new(0, 8, 0, 0)
        $View.Body.Controls.Add($summary)
    }

    $canNext = ($null -ne $script:preview -and $script:preview.CanInstall)
    $next = New-Button '查看清单' 'primary' 112 32
    if (-not $canNext) { $next.Enabled = $false; $next.BackColor = $script:kSurface3; $next.ForeColor = $script:kText3 }
    Set-Footer $View '返回控制台' { Show-View 'main' } @(
        @{ Button = (New-Button '上一步' 'secondary' 96 32); Action = { Go-SetupStep 1 } },
        @{ Button = $next; Action = { Go-SetupStep 3 } }
    )
}

function Build-SetupStep3($View) {
    $View.Body.Controls.Add((New-SectionTitle '文件清单' $(if ($script:preview) { "版本 $($script:preview.ManifestVersion)" } else { '' })))
    if ($null -eq $script:preview) {
        $View.Body.Controls.Add((New-Label '没有可用的预检结果，请返回上一步。' $script:fBody $script:kWarn))
        Set-Footer $View '返回控制台' { Show-View 'main' } @(@{ Button = (New-Button '上一步' 'secondary' 96 32); Action = { Go-SetupStep 2 } })
        return
    }
    $groups = @(
        @{ Title = '新增'; Note = '客户端原本没有，卸载时删除'; Filter = { [string]$_.PreState -eq 'absent' } },
        @{ Title = '替换'; Note = '先备份原件，卸载时还原'; Filter = { [string]$_.PreState -eq 'present' } },
        @{ Title = '跳过'; Note = '内容一致或可选缺失，不动'; Filter = { -not ($_.PreState -in @('absent', 'present')) } }
    )
    foreach ($group in $groups) {
        $items = @($script:preview.Items | Where-Object $group.Filter)
        if ($items.Count -eq 0) { continue }
        $View.Body.Controls.Add((New-SectionTitle $group.Title ($items.Count.ToString() + ' 项 · ' + $group.Note)))
        $color = switch ($group.Title) { '新增' { $script:kJade } '替换' { $script:kAccent } default { $script:kText3 } }
        $card = New-Card ($items.Count * 34 + 8) $script:kSurface $script:kLineSoft
        $y = 4
        foreach ($item in $items) {
            $tag = switch ($group.Title) { '新增' { '新增' } '替换' { '备份后替换' } default { '跳过' } }
            $note = if ([string]$item.PreState -eq 'missing-optional') { '可选组件缺失' } else { '' }
            $row = New-FileRow ([string]$item.Target) $tag $color $note
            $row.Location = [Drawing.Point]::new(0, $y)
            $card.Controls.Add($row)
            $y += 34
        }
        $View.Body.Controls.Add($card)
    }

    $issues = @($script:preview.Items | Where-Object { $null -ne $_.Issue })
    if ($issues.Count -gt 0) {
        $View.Body.Controls.Add((New-SectionTitle '冲突' ($issues.Count.ToString() + ' 项需要处理')))
        $card = New-Card ($issues.Count * 34 + 8) $script:kSurface $script:kWarn
        $y = 4
        foreach ($item in $issues) {
            $row = New-FileRow ([string]$item.Target) '冲突' $script:kWarn ([string]$item.Issue)
            $row.Location = [Drawing.Point]::new(0, $y)
            $card.Controls.Add($row)
            $y += 34
        }
        $View.Body.Controls.Add($card)
    }

    $backupNote = New-Card 44 (C '211A10') $script:kLineSoft
    $note = New-Label '替换的原件备份到客户端内 host64\dlss5-backup，卸载时按安装记录还原。' $script:fSmall $script:kText3
    $note.Location = [Drawing.Point]::new(16, 13)
    $backupNote.Controls.Add($note)
    $View.Body.Controls.Add($backupNote)

    $start = New-Button '开始安装' 'primary' 112 32
    if (-not $script:preview.CanInstall) { $start.Enabled = $false; $start.BackColor = $script:kSurface3; $start.ForeColor = $script:kText3 }
    Set-Footer $View '返回控制台' { Show-View 'main' } @(
        @{ Button = (New-Button '上一步' 'secondary' 96 32); Action = { Go-SetupStep 2 } },
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
    $View.Body.Controls.Add((New-SectionTitle '安装' '正在写入客户端'))
    $card = New-Card 96 $script:kSurface $script:kLineSoft
    $label = New-Label '正在安装…' $script:fBody $script:kAccent
    $label.Location = [Drawing.Point]::new(18, 18)
    $card.Controls.Add($label)
    $note = New-Label '替换前先备份原件；完成后回读每个文件校验哈希。' $script:fSmall $script:kText3
    $note.Location = [Drawing.Point]::new(18, 44)
    $card.Controls.Add($note)
    $bar = [Windows.Forms.Panel]::new()
    $bar.Location = [Drawing.Point]::new(18, 70)
    $bar.Width = $ContentWidth - 36
    $bar.Height = 4
    $bar.BackColor = $script:kLineSoft
    $fill = [Windows.Forms.Panel]::new()
    $fill.Location = [Drawing.Point]::new(0, 0)
    $fill.Width = [int](($ContentWidth - 36) * 0.6)
    $fill.Height = 4
    $fill.BackColor = $script:kAccent
    $bar.Controls.Add($fill)
    $card.Controls.Add($bar)
    $View.Body.Controls.Add($card)
    Set-Footer $View '' $null @(@{ Button = (New-Button '返回控制台' 'secondary' 112 32); Action = { Show-View 'main' } })
}

function Build-SetupStep5($View) {
    $done = ($null -ne $script:installResult -and $null -eq $script:installError)
    $View.Body.Controls.Add((New-SectionTitle '完成' ''))
    $card = New-Card 96 $script:kSurface $script:kLineSoft
    $badge = New-Label $(if ($done) { '✓' } else { '!' }) ([Drawing.Font]::new('Microsoft YaHei UI', 18)) $(if ($done) { $script:kJade } else { $script:kWarn })
    $badge.Location = [Drawing.Point]::new(18, 16)
    $card.Controls.Add($badge)
    $title = New-Label $(if ($done) { '安装完成' } else { '安装未完成' }) ([Drawing.Font]::new('Microsoft YaHei UI', 12, [Drawing.FontStyle]::Bold)) $(if ($done) { $script:kText } else { $script:kWarn })
    $title.Location = [Drawing.Point]::new(46, 18)
    $card.Controls.Add($title)
    $summary = if ($done) {
        if ($script:installResult.Status -eq 'already_current') { '客户端已是最新，没有写入文件。' } else { '已写入 ' + $script:installResult.Count + ' 个文件，回读校验通过。' }
    } else { $script:installError }
    $detail = New-Label $summary $script:fSmall $script:kText2
    $detail.Location = [Drawing.Point]::new(18, 52)
    $detail.MaximumSize = [Drawing.Size]::new(($ContentWidth - 36), 40)
    $card.Controls.Add($detail)
    $View.Body.Controls.Add($card)

    $deploy = Get-DeployInfo
    $info = New-Card 100 $script:kSurface $script:kLineSoft
    $rows = @(
        @('客户端', $script:clientPath),
        @('版本', $(if ($deploy.Version) { $deploy.Version } else { '—' })),
        @('备份', $(if ($deploy.BackupCount -gt 0) { "host64\dlss5-backup（$($deploy.BackupCount) 个文件）" } else { '无需备份' }))
    )
    $y = 6
    foreach ($pair in $rows) {
        $row = New-KvRow $pair[0] ([string]$pair[1])
        $row.Location = [Drawing.Point]::new(0, $y)
        $info.Controls.Add($row)
        $y += 30
    }
    $View.Body.Controls.Add($info)

    $hint = New-Label '现在可以启动游戏；需要修复、备份或卸载时进入维护页。' $script:fSmall $script:kText3 520
    $hint.Margin = [Windows.Forms.Padding]::new(0, 8, 0, 0)
    $View.Body.Controls.Add($hint)

    $maint = New-Button '查看维护' 'secondary' 112 32
    $maint.Add_Click({ Show-View 'maint' })
    $doneButton = New-Button '返回控制台' 'primary' 112 32
    $doneButton.Add_Click({ Show-View 'main' })
    Set-Footer $View '' $null @(@{ Button = $maint }, @{ Button = $doneButton })
}

function Update-Maint {
    $view = $maintView
    Clear-Panel $view.Extra
    Clear-Panel $view.Body
    $deploy = Get-DeployInfo

    $view.Body.Controls.Add((New-SectionTitle '已安装' $(if ($deploy.Installed) { '组件正常' } else { '未安装' })))
    $info = New-Card 124 $script:kSurface $script:kLineSoft
    $rows = @(
        @('客户端', $script:clientPath),
        @('版本', $(if ($deploy.Installed) { $deploy.Version + ' · ' + $deploy.InstalledAt } else { '—' })),
        @('文件', $(if ($deploy.Installed) { "新增 $($deploy.NewFiles) · 替换 $($deploy.Replaced) · 跳过 $($deploy.Same)" } else { '—' })),
        @('备份', $(if ($deploy.BackupCount -gt 0) { "host64\dlss5-backup（$($deploy.BackupCount) 个文件）" } elseif ($deploy.Installed) { '无需备份' } else { '—' }))
    )
    $y = 8
    foreach ($pair in $rows) {
        $row = New-KvRow $pair[0] ([string]$pair[1])
        $row.Location = [Drawing.Point]::new(0, $y)
        $info.Controls.Add($row)
        $y += 28
    }
    $view.Body.Controls.Add($info)

    $view.Body.Controls.Add((New-SectionTitle '组件' ''))
    $files = @()
    try { $files = @((Get-Dlss5Status -Client $script:clientPath).Files) } catch { Add-Log ('读取组件状态失败：' + $_.Exception.Message) }
    $shown = @($files | Where-Object { $_.Name -in @('d3d9.dll', 'standalone-dlssnr.addon32', 'host64/AIO DLSS5 32-bit Wrapper.exe', 'host64/nvngx.dll', 'host64/nvngx_dlss.dll', 'host64/nvngx_dlssg.dll') })
    if ($shown.Count -eq 0) { $shown = @($files | Select-Object -First 5) }
    $componentCard = New-Card ([Math]::Max(44, $shown.Count * 36 + 8)) $script:kSurface $script:kLineSoft
    $y = 4
    foreach ($file in $shown) {
        $ok = ([string]$file.Status -eq '与候选一致')
        $row = New-CheckRow $ok ([string]$file.Name) ([string]$file.Status)
        $row.Location = [Drawing.Point]::new(0, $y)
        $componentCard.Controls.Add($row)
        $y += 36
    }
    if ($shown.Count -eq 0) {
        $empty = New-Label '没有可读取的组件状态。' $script:fBody $script:kText3
        $empty.Location = [Drawing.Point]::new(16, 14)
        $componentCard.Controls.Add($empty)
    }
    $view.Body.Controls.Add($componentCard)

    $actions = [Windows.Forms.FlowLayoutPanel]::new()
    $actions.Width = $ContentWidth
    $actions.Height = 40
    $actions.WrapContents = $true
    $actions.BackColor = [Drawing.Color]::Transparent
    $repair = New-Button '修复安装' 'secondary' 96 32
    $repair.Add_Click({ Invoke-Ui {
        $ops = @(Get-Dlss5Operations -Client $script:clientPath | Where-Object { $_.Status -eq 'completed' })
        if ($ops.Count -eq 0) { throw '没有可修复的安装事务记录；可改用重新安装。' }
        $result = Invoke-Dlss5Repair -Operation ([string]$ops[$ops.Count - 1].Operation)
        Add-Log ("修复状态：" + $result.Status + "，修复文件 " + $result.Repaired + " 项。")
        Add-Log ('记录：' + $result.Report)
        if (@($result.Errors).Count) { Add-Log (@($result.Errors) -join '；') }
    } })
    $openBackup = New-Button '打开备份目录' 'secondary' 112 32
    $openBackup.Add_Click({ Invoke-Ui {
        $backup = Join-Path $script:clientPath 'host64/dlss5-backup'
        if (-not [IO.Directory]::Exists($backup)) { throw '当前客户端没有备份目录。' }
        Start-Process -FilePath 'explorer.exe' -ArgumentList @(('"' + $backup + '"'))
        Add-Log ('已请求打开：' + $backup)
    } })
    $refresh = New-Button '检查状态' 'secondary' 96 32
    $refresh.Add_Click({ Invoke-Ui { Refresh-Status; Update-Maint } })
    $advanced = New-Button $(if ($script:showAdvanced) { '收起高级工具' } else { '高级工具' }) 'secondary' 112 32
    $advanced.Add_Click({ $script:showAdvanced = -not $script:showAdvanced; Update-Maint })
    $actions.Controls.Add($refresh)
    $actions.Controls.Add($repair)
    $actions.Controls.Add($openBackup)
    $actions.Controls.Add($advanced)
    $view.Body.Controls.Add($actions)

    if ($script:showAdvanced) {
        $view.Body.Controls.Add((New-SectionTitle '高级工具' '档位、诊断与运行日志'))
        $schemes = @()
        try { $schemes = @(Get-Dlss5Schemes) } catch { }
        $combo = [Windows.Forms.ComboBox]::new()
        $combo.DropDownStyle = 'DropDownList'
        $combo.Width = 200
        $combo.DisplayMember = 'Name'
        $combo.BackColor = $script:kSurface2
        $combo.ForeColor = $script:kText
        foreach ($scheme in $schemes) { $null = $combo.Items.Add($scheme) }
        if ($combo.Items.Count -gt 0) { $combo.SelectedIndex = 0 }
        $apply = New-Button '应用档位' 'secondary' 96 26
        $apply.Add_Click(({
            if ($combo.SelectedIndex -lt 0) { Add-Log '请先选择档位。'; return }
            Invoke-Ui {
                $picked = $combo.SelectedItem
                $result = Apply-Dlss5Scheme -Client $script:clientPath -Path $picked.Path
                Add-Log ("已应用档位【" + $picked.Name + "】：" + $result.Status + "；重启游戏后生效。")
            }
        }).GetNewClosure())
        $read = New-Button '读取当前值' 'secondary' 104 26
        $read.Add_Click({ Invoke-Ui {
            $advancedValues = Get-Dlss5AdvancedSettings -Client $script:clientPath
            Add-Log ("当前配置：NR=" + $advancedValues.NeuralRendering + "，模型=" + $advancedValues.Model + "，强度=" + $advancedValues.Intensity + "，色调=" + $advancedValues.LocalTone + "，结构=" + $advancedValues.LocalStructure + "，皮肤=" + $advancedValues.SkinStructure)
        } })
        $schemeRow = [Windows.Forms.FlowLayoutPanel]::new()
        $schemeRow.Width = $ContentWidth
        $schemeRow.Height = 40
        $schemeRow.WrapContents = $false
        $schemeRow.BackColor = [Drawing.Color]::Transparent
        $schemeRow.Controls.Add($combo)
        $schemeRow.Controls.Add($apply)
        $schemeRow.Controls.Add($read)
        $view.Body.Controls.Add($schemeRow)

        $saveName = [Windows.Forms.TextBox]::new()
        $saveName.Width = 150
        $saveName.Height = 26
        $saveName.BackColor = $script:kSurface2
        $saveName.ForeColor = $script:kText
        $saveName.BorderStyle = 'FixedSingle'
        $saveScheme = New-Button '保存为方案' 'secondary' 104 26
        $saveScheme.Add_Click(({
            if (-not $saveName.Text.Trim()) { Add-Log '请先填写方案名（1-24 字符）。'; return }
            Invoke-Ui {
                $path = Save-Dlss5Scheme -Client $script:clientPath -Name $saveName.Text
                Add-Log ('方案已保存：' + $path)
            }
        }).GetNewClosure())
        $saveRow = [Windows.Forms.FlowLayoutPanel]::new()
        $saveRow.Width = $ContentWidth
        $saveRow.Height = 40
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
        $nrBox.Margin = [Windows.Forms.Padding]::new(0, 7, 16, 0)
        $modelBox = [Windows.Forms.ComboBox]::new()
        $modelBox.DropDownStyle = 'DropDownList'
        $modelBox.Width = 96
        $modelBox.BackColor = $script:kSurface2
        $modelBox.ForeColor = $script:kText
        foreach ($name in @('模型 1', '模型 2', '模型 3')) { $null = $modelBox.Items.Add($name) }
        $modelBox.SelectedIndex = 0
        $modelBox.Margin = [Windows.Forms.Padding]::new(0, 2, 0, 0)
        $modelLabel = New-Label '模型' $script:fSmall $script:kText3
        $modelLabel.Margin = [Windows.Forms.Padding]::new(0, 8, 6, 0)
        $paramRow1 = [Windows.Forms.FlowLayoutPanel]::new()
        $paramRow1.Width = $ContentWidth
        $paramRow1.Height = 34
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
            $box.Width = 62
            $box.BackColor = $script:kSurface2
            $box.ForeColor = $script:kText
            $box.BorderStyle = 'FixedSingle'
            $box.Font = $script:fBody
            $box.Margin = [Windows.Forms.Padding]::new(0, 4, 10, 0)
            return $box
        }
        $intensityBox = New-NumBox 0 1 0.05 1
        $toneBox = New-NumBox 0 2 0.05 1
        $structureBox = New-NumBox 0 1 0.05 1
        $skinBox = New-NumBox -1 1 0.1 -1
        $paramRow2 = [Windows.Forms.FlowLayoutPanel]::new()
        $paramRow2.Width = $ContentWidth
        $paramRow2.Height = 36
        $paramRow2.WrapContents = $false
        $paramRow2.BackColor = [Drawing.Color]::Transparent
        foreach ($pair in @(@('强度', $intensityBox), @('色调', $toneBox), @('结构', $structureBox), @('皮肤', $skinBox))) {
            $caption = New-Label $pair[0] $script:fSmall $script:kText3
            $caption.Margin = [Windows.Forms.Padding]::new(0, 8, 4, 0)
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
        $paramRow3.Height = 32
        $paramRow3.WrapContents = $false
        $paramRow3.BackColor = [Drawing.Color]::Transparent
        $paramRow3.Controls.Add($detachedBox)
        $paramRow3.Controls.Add($flowBox)
        $view.Body.Controls.Add($paramRow3)

        $readSettings = New-Button '填入当前值' 'secondary' 104 30
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
        $saveSettings = New-Button '保存设置' 'primary' 96 30
        $saveSettings.Add_Click(({
            Invoke-Ui {
                $result = Set-Dlss5AdvancedSettings -Client $script:clientPath -NeuralRendering $nrBox.Checked `
                    -Model ($modelBox.SelectedIndex + 1) -Intensity ([double]$intensityBox.Value) `
                    -LocalTone ([double]$toneBox.Value) -LocalStructure ([double]$structureBox.Value) -SkinStructure ([double]$skinBox.Value) `
                    -DetachedPresentation $detachedBox.Checked -OpticalFlow $flowBox.Checked
                Add-Log ("设置状态：" + $result.Status + "；备份：" + $result.Backup + "；下次启动游戏生效。")
            }
        }).GetNewClosure())
        $resetSettings = New-Button '恢复候选默认' 'secondary' 112 30
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
        $paramRow4.Height = 38
        $paramRow4.WrapContents = $false
        $paramRow4.BackColor = [Drawing.Color]::Transparent
        $paramRow4.Controls.Add($readSettings)
        $paramRow4.Controls.Add($saveSettings)
        $paramRow4.Controls.Add($resetSettings)
        $view.Body.Controls.Add($paramRow4)
        $tools = [Windows.Forms.FlowLayoutPanel]::new()
        $tools.Width = $ContentWidth
        $tools.Height = 80
        $tools.WrapContents = $true
        $tools.BackColor = [Drawing.Color]::Transparent
        $diagnose = New-Button '导出诊断' 'secondary' 96 30
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
        $restore = New-Button '恢复安装前文件' 'secondary' 128 30
        $restore.Add_Click({ Invoke-Ui {
            $ops = @(Get-Dlss5Operations -Client $script:clientPath | Where-Object { $_.Status -eq 'completed' })
            if ($ops.Count -eq 0) { throw '没有可恢复的安装事务记录。' }
            $result = Invoke-Dlss5Restore -Operation ([string]$ops[$ops.Count - 1].Operation)
            Add-Log ("恢复状态：" + $result.Status + "；记录：" + $result.Report)
            Update-Maint
        } })
        $exportList = New-Button '导出移入清单' 'secondary' 112 30
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
        $openPanel = New-Button '启动游戏内面板' 'secondary' 128 30
        $openPanel.Add_Click({ Invoke-Ui {
            $panelExe = Join-Path $PSScriptRoot 'bin/dlss5-panel.exe'
            if (-not [IO.File]::Exists($panelExe)) { throw '面板程序缺失：bin/dlss5-panel.exe' }
            $logDir = Join-Path $project ('_scratch/dlss5-aio/panel-logs/' + [guid]::NewGuid().ToString('N'))
            [IO.Directory]::CreateDirectory($logDir) | Out-Null
            Start-Process -FilePath $panelExe -ArgumentList @('--game', ('"' + (Join-Path $script:clientPath 'wow.exe') + '"')) -WindowStyle Hidden -RedirectStandardOutput (Join-Path $logDir 'stdout.log') -RedirectStandardError (Join-Path $logDir 'stderr.log')
            $script:lastPanelLog = $logDir
            Add-Log ('游戏内面板已启动。日志：' + $logDir)
        } })
        $watch = New-Button '启动鼠标助手' 'secondary' 112 30
        $watch.Add_Click({ Invoke-Ui {
            $result = Start-Dlss5WindowWatch -Client $script:clientPath -Watcher $script:watcherPath
            $script:lastWatchLog = $result.LogDirectory
            Add-Log ($result.Message + '；日志：' + $result.LogDirectory)
        } })
        $pipeline = New-Button '切换 AIO 管线' 'secondary' 112 30
        $pipeline.Add_Click({ Invoke-Ui {
            $status = Get-Dlss5Status -Client $script:clientPath
            if ($status.Config.PipelineEnabled -notin @('0', '1')) { throw '无法确定当前 AIO 管线开关。' }
            $enable = $status.Config.PipelineEnabled -eq '0'
            $result = Set-Dlss5Pipeline -Client $script:clientPath -Enabled $enable
            Add-Log ("已保存：" + $(if ($enable) { '启用' } else { '停用' }) + " AIO 管线；重启游戏后生效。")
        } })
        foreach ($tool in @($diagnose, $restore, $exportList, $openPanel, $watch, $pipeline)) { $tools.Controls.Add($tool) }
        $view.Body.Controls.Add($tools)

        $maintLog = [Windows.Forms.TextBox]::new()
        $maintLog.Multiline = $true
        $maintLog.ReadOnly = $true
        $maintLog.ScrollBars = 'Vertical'
        $maintLog.Width = $ContentWidth
        $maintLog.Height = 110
        $maintLog.BackColor = $script:kSurface
        $maintLog.ForeColor = $script:kText2
        $maintLog.BorderStyle = 'FixedSingle'
        $maintLog.Font = $script:fMono
        $maintLog.Text = $script:logText
        $maintLog.Margin = [Windows.Forms.Padding]::new(0, 4, 0, 0)
        $script:logBox = $maintLog
        $view.Body.Controls.Add($maintLog)
    }

    $danger = New-Card 124 (C '1E1512') $script:kDanger
    $dangerTitle = New-Label '移除组件' $script:fBold $script:kWarn
    $dangerTitle.Location = [Drawing.Point]::new(18, 14)
    $danger.Controls.Add($dangerTitle)
    $dangerText = New-Label '卸载只处理安装时写入的文件：新增的删除，替换的从备份还原，跳过的保持原样。用户配置、日志与存档默认保留。' $script:fSmall $script:kText3
    $dangerText.Location = [Drawing.Point]::new(18, 40)
    $dangerText.MaximumSize = [Drawing.Size]::new(($ContentWidth - 190), 60)
    $danger.Controls.Add($dangerText)
    $uninstall = New-Button '查看卸载清单' 'danger' 118 32
    $uninstall.Location = [Drawing.Point]::new(384, 46)
    $uninstall.Add_Click({ if ((Get-DeployInfo).Installed) { $script:unResult = $null; Show-View 'uninstall' } else { Add-Log '当前客户端没有安装记录，无法卸载。' } })
    $danger.Controls.Add($uninstall)
    $view.Body.Controls.Add($danger)

    Set-Footer $view '返回控制台' { Show-View 'main' } @()
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
                $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
                if ($hash -ne [string]$entry.sha256) { $modified += $entry }
            }
        } catch { }
    }

    if ($modified.Count -gt 0) {
        $warnCard = New-Card 52 (C '231811') $script:kWarn
        $warnText = New-Label ("有 " + $modified.Count + " 个替换文件已被改动，还原会覆盖当前内容：" + (($modified | ForEach-Object { $_.target }) -join '，')) $script:fSmall $script:kWarn
        $warnText.Location = [Drawing.Point]::new(16, 16)
        $warnText.MaximumSize = [Drawing.Size]::new(($ContentWidth - 32), 40)
        $warnCard.Controls.Add($warnText)
        $view.Body.Controls.Add($warnCard)
    }

    $view.Body.Controls.Add((New-SectionTitle '将删除' ($remove.Count.ToString() + ' 个新增文件')))
    $card = New-Card ([Math]::Max(40, $remove.Count * 34 + 8)) $script:kSurface $script:kLineSoft
    $y = 4
    foreach ($entry in $remove) {
        $row = New-FileRow ([string]$entry.target) '删除' $script:kDanger
        $row.Location = [Drawing.Point]::new(0, $y)
        $card.Controls.Add($row)
        $y += 34
    }
    if ($remove.Count -eq 0) {
        $none = New-Label '无' $script:fSmall $script:kText3
        $none.Location = [Drawing.Point]::new(16, 12)
        $card.Controls.Add($none)
    }
    $view.Body.Controls.Add($card)

    $view.Body.Controls.Add((New-SectionTitle '将还原' ($restore.Count.ToString() + ' 个替换文件')))
    $card = New-Card ([Math]::Max(40, $restore.Count * 34 + 8)) $script:kSurface $script:kLineSoft
    $y = 4
    foreach ($entry in $restore) {
        $row = New-FileRow ([string]$entry.target) '还原' $script:kAccent
        $row.Location = [Drawing.Point]::new(0, $y)
        $card.Controls.Add($row)
        $y += 34
    }
    if ($restore.Count -eq 0) {
        $none = New-Label '无' $script:fSmall $script:kText3
        $none.Location = [Drawing.Point]::new(16, 12)
        $card.Controls.Add($none)
    }
    $view.Body.Controls.Add($card)

    $residue = @()
    try { $residue = @($deploy.Record.Record.residue | Where-Object { [string]$_.policy -ne 'delete' } | ForEach-Object { [string]$_.path }) } catch { }
    $keepCount = $keep.Count + $residue.Count + 2
    $view.Body.Controls.Add((New-SectionTitle '将保留' ($keepCount.ToString() + ' 项，含用户数据')))
    $card = New-Card ([Math]::Max(40, ($keep.Count + $residue.Count + 2) * 34 + 8)) $script:kSurface $script:kLineSoft
    $y = 4
    foreach ($entry in $keep) {
        $row = New-FileRow ([string]$entry.target) '保留' $script:kText3
        $row.Location = [Drawing.Point]::new(0, $y)
        $card.Controls.Add($row)
        $y += 34
    }
    foreach ($path in $residue) {
        $row = New-FileRow $path '保留' $script:kText3
        $row.Location = [Drawing.Point]::new(0, $y)
        $card.Controls.Add($row)
        $y += 34
    }
    foreach ($label in @('用户配置', '日志与存档')) {
        $row = New-FileRow $label '保留' $script:kText3
        $row.Location = [Drawing.Point]::new(0, $y)
        $card.Controls.Add($row)
        $y += 34
    }
    $view.Body.Controls.Add($card)

    $note = New-Label '卸载成功后同时清理安装记录与 host64\dlss5-backup 备份目录；失败不掩盖，备份缺失时不删当前文件。' $script:fSmall $script:kText3 520
    $note.Margin = [Windows.Forms.Padding]::new(0, 6, 0, 0)
    $view.Body.Controls.Add($note)

    if ($null -ne $script:unResult) {
        $view.Body.Controls.Add((New-SectionTitle '卸载结果' ''))
        $result = $script:unResult
        $card = New-Card 128 $script:kSurface $(if (@($result.Errors).Count) { $script:kWarn } else { $script:kLineSoft })
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
            $row.Location = [Drawing.Point]::new(0, $y)
            $card.Controls.Add($row)
            $y += 24
        }
        $view.Body.Controls.Add($card)
        if (@($result.Errors).Count) {
            $errorText = New-Label (@($result.Errors) -join '；') $script:fSmall $script:kWarn 520
            $view.Body.Controls.Add($errorText)
        }
    }

    $run = New-Button '确认卸载' 'danger' 112 32
    if ($null -ne $script:unResult) { $run.Enabled = $false; $run.BackColor = $script:kSurface3; $run.ForeColor = $script:kText3 }
    $run.Add_Click({ Invoke-Ui {
        $script:unResult = Invoke-Dlss5Uninstall -Client $script:clientPath
        Add-Log ("卸载状态：" + $script:unResult.Status + "；还原 " + $script:unResult.Restored + " 项，删除 " + $script:unResult.Deleted + " 项。")
        Update-Uninstall
    } })
    Set-Footer $view '返回维护' { Show-View 'maint' } @(@{ Button = $run })
}

function Invoke-Ui([scriptblock]$Action) {
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
        $exe = Join-Path $script:clientPath 'wow.exe'
        if (-not [IO.File]::Exists($exe)) { return }
        $gamePath = [IO.Path]::GetFullPath($exe)
        $procs = @(Get-CimInstance Win32_Process -Filter "Name = 'wow.exe'" -ErrorAction Stop |
            Where-Object { $_.ExecutablePath -and $_.ExecutablePath.Equals($gamePath, [StringComparison]::OrdinalIgnoreCase) })
        if ($procs.Count -eq 0) { return }
        $panelProcs = @(Get-CimInstance Win32_Process -Filter "Name = 'dlss5-panel.exe'" -ErrorAction Stop)
        if ($panelProcs.Count -gt 0) { return }
        $panelExe = Join-Path $PSScriptRoot 'bin/dlss5-panel.exe'
        if (-not [IO.File]::Exists($panelExe)) { return }
        $logDir = Join-Path $project ('_scratch/dlss5-aio/panel-logs/' + [guid]::NewGuid().ToString('N'))
        [IO.Directory]::CreateDirectory($logDir) | Out-Null
        Start-Process -FilePath $panelExe -ArgumentList @('--game', ('"' + (Join-Path $script:clientPath 'wow.exe') + '"')) -WindowStyle Hidden -RedirectStandardOutput (Join-Path $logDir 'stdout.log') -RedirectStandardError (Join-Path $logDir 'stderr.log')
        $script:lastPanelLog = $logDir
        Add-Log '检测到游戏运行但面板未启动，已自动拉起。'
    } catch {
        Add-Log ('面板自动拉起未执行：' + $_.Exception.Message)
    }
})
$watchTimer.Start()
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
if ($Smoke) {
    $form.Dispose()
    Write-Output 'smoke ok'
    return
}
$null = $form.ShowDialog()
$form.Dispose()
