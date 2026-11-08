param(
    [string]$Client,
    [string]$Stage,
    [string]$Watcher
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

$form = [Windows.Forms.Form]::new()
$form.Text = 'WoW 3.3.5 DLSS5 控制中心（开发候选）'
$form.StartPosition = 'CenterScreen'
$form.ClientSize = [Drawing.Size]::new(980, 720)
$form.MinimumSize = [Drawing.Size]::new(820, 640)
$form.AutoScaleMode = [Windows.Forms.AutoScaleMode]::Dpi

$layout = [Windows.Forms.TableLayoutPanel]::new()
$layout.Dock = 'Fill'
$layout.Padding = [Windows.Forms.Padding]::new(12)
$layout.ColumnCount = 1
$layout.RowCount = 8
foreach ($height in @(28, 35, 35, 60, 42, 72, 26)) {
    $layout.RowStyles.Add([Windows.Forms.RowStyle]::new([Windows.Forms.SizeType]::Absolute, $height)) | Out-Null
}
$layout.RowStyles.Add([Windows.Forms.RowStyle]::new([Windows.Forms.SizeType]::Percent, 100)) | Out-Null
$form.Controls.Add($layout)

$heading = [Windows.Forms.Label]::new()
$heading.Text = 'AIO 2.2.4 / ReShade 6.8.0    开发候选：仅允许修改隔离客户端，游戏验收尚未完成'
$heading.Dock = 'Fill'
$layout.Controls.Add($heading, 0, 0)

function New-PathRow([string]$Caption, [string]$Value) {
    $row = [Windows.Forms.TableLayoutPanel]::new()
    $row.Dock = 'Fill'; $row.ColumnCount = 3; $row.RowCount = 1
    $row.ColumnStyles.Add([Windows.Forms.ColumnStyle]::new([Windows.Forms.SizeType]::Absolute, 85)) | Out-Null
    $row.ColumnStyles.Add([Windows.Forms.ColumnStyle]::new([Windows.Forms.SizeType]::Percent, 100)) | Out-Null
    $row.ColumnStyles.Add([Windows.Forms.ColumnStyle]::new([Windows.Forms.SizeType]::Absolute, 75)) | Out-Null
    $label = [Windows.Forms.Label]::new(); $label.Text = $Caption; $label.Dock = 'Fill'
    $box = [Windows.Forms.TextBox]::new(); $box.Text = $Value; $box.Dock = 'Fill'
    $choose = [Windows.Forms.Button]::new(); $choose.Text = '选择'; $choose.Dock = 'Fill'
    $row.Controls.Add($label, 0, 0); $row.Controls.Add($box, 1, 0); $row.Controls.Add($choose, 2, 0)
    $choose.Add_Click({
        $dialog = [Windows.Forms.FolderBrowserDialog]::new()
        try {
            if ([IO.Directory]::Exists($box.Text)) { $dialog.SelectedPath = $box.Text }
            if ($dialog.ShowDialog($form) -eq [Windows.Forms.DialogResult]::OK) { $box.Text = $dialog.SelectedPath }
        } finally { $dialog.Dispose() }
    }.GetNewClosure())
    return [pscustomobject]@{ Row = $row; Box = $box }
}

$clientRow = New-PathRow '客户端' $Client
$stageRow = New-PathRow '组件来源' $Stage
$layout.Controls.Add($clientRow.Row, 0, 1)
$layout.Controls.Add($stageRow.Row, 0, 2)

$settings = [Windows.Forms.FlowLayoutPanel]::new()
$settings.Dock = 'Fill'; $settings.WrapContents = $true; $settings.AutoScroll = $true
$nr = [Windows.Forms.CheckBox]::new(); $nr.Text = '启用 NR'; $nr.AutoSize = $true; $nr.Checked = $true
$modelLabel = [Windows.Forms.Label]::new(); $modelLabel.Text = '模型'; $modelLabel.AutoSize = $true
$model = [Windows.Forms.ComboBox]::new(); $model.DropDownStyle = 'DropDownList'; $model.Width = 110
$null = $model.Items.Add('模型 1'); $null = $model.Items.Add('模型 2（待验收）'); $null = $model.Items.Add('模型 3（待验收）')
$model.SelectedIndex = 0
$intensityLabel = [Windows.Forms.Label]::new(); $intensityLabel.Text = 'NR 强度'; $intensityLabel.AutoSize = $true
$intensity = [Windows.Forms.NumericUpDown]::new(); $intensity.Minimum = 0; $intensity.Maximum = 2
$intensity.DecimalPlaces = 2; $intensity.Increment = [decimal]0.05; $intensity.Value = 1; $intensity.Width = 70
$detached = [Windows.Forms.CheckBox]::new(); $detached.Text = '独立输出候选（同时启动鼠标助手）'; $detached.AutoSize = $true
$flow = [Windows.Forms.CheckBox]::new(); $flow.Text = 'Optical Flow（试验）'; $flow.AutoSize = $true
$toneLabel = [Windows.Forms.Label]::new(); $toneLabel.Text = '色调结构'; $toneLabel.AutoSize = $true
$tone = [Windows.Forms.NumericUpDown]::new(); $tone.Minimum = 0; $tone.Maximum = 2; $tone.DecimalPlaces = 2; $tone.Increment = [decimal]0.05; $tone.Value = 1; $tone.Width = 70
$structureLabel = [Windows.Forms.Label]::new(); $structureLabel.Text = '结构强度'; $structureLabel.AutoSize = $true
$structure = [Windows.Forms.NumericUpDown]::new(); $structure.Minimum = 0; $structure.Maximum = 2; $structure.DecimalPlaces = 2; $structure.Increment = [decimal]0.05; $structure.Value = 1; $structure.Width = 70
$skinLabel = [Windows.Forms.Label]::new(); $skinLabel.Text = '皮肤结构'; $skinLabel.AutoSize = $true
$skin = [Windows.Forms.NumericUpDown]::new(); $skin.Minimum = -1; $skin.Maximum = 1; $skin.DecimalPlaces = 2; $skin.Increment = [decimal]0.1; $skin.Value = -1; $skin.Width = 70
$advNote = [Windows.Forms.Label]::new(); $advNote.Text = '—— 档位与微调（改后需重启游戏；游戏内 Home → Add-ons 可实时微调）——'; $advNote.AutoSize = $true; $advNote.ForeColor = [Drawing.Color]::SteelBlue
foreach ($control in @($nr, $modelLabel, $model, $intensityLabel, $intensity, $toneLabel, $tone, $structureLabel, $structure, $skinLabel, $skin, $detached, $flow, $advNote)) { $settings.Controls.Add($control) }
$layout.Controls.Add($settings, 0, 3)

$actions = [Windows.Forms.FlowLayoutPanel]::new(); $actions.Dock = 'Fill'; $actions.AutoScroll = $true
$actions2 = [Windows.Forms.FlowLayoutPanel]::new(); $actions2.Dock = 'Fill'; $actions2.AutoScroll = $true
$layout.Controls.Add($actions, 0, 4); $layout.Controls.Add($actions2, 0, 5)
function New-Action([Windows.Forms.FlowLayoutPanel]$Panel, [string]$Text) {
    $button = [Windows.Forms.Button]::new(); $button.Text = $Text; $button.AutoSize = $true
    $Panel.Controls.Add($button); return $button
}
$check = New-Action $actions '检查状态'
$preview = New-Action $actions '预览安装'
$install = New-Action $actions '安装到隔离客户端'
$save = New-Action $actions '保存设置'
$reset = New-Action $actions '恢复候选默认'
$watch = New-Action $actions2 '启动鼠标助手'
$watchLog = New-Action $actions2 '查看助手日志'
$panelLog = New-Action $actions2 '查看面板日志'
$diagnose = New-Action $actions2 '导出诊断'
$pipeline = New-Action $actions2 '切换 AIO 管线'
$repair = New-Action $actions2 '修复文件'
$restore = New-Action $actions2 '恢复安装前文件'
$operations = [Windows.Forms.ComboBox]::new(); $operations.DropDownStyle = 'DropDownList'; $operations.Width = 220
$operations.DisplayMember = 'Label'
$actions2.Controls.Add($operations)
$schemeLabel = [Windows.Forms.Label]::new(); $schemeLabel.Text = '档位'; $schemeLabel.AutoSize = $true
$schemeCombo = [Windows.Forms.ComboBox]::new(); $schemeCombo.DropDownStyle = 'DropDownList'; $schemeCombo.Width = 140; $schemeCombo.DisplayMember = 'Name'
$applyScheme = [Windows.Forms.Button]::new(); $applyScheme.Text = '应用档位'; $applyScheme.AutoSize = $true
$schemeName = [Windows.Forms.TextBox]::new(); $schemeName.Width = 100
$saveScheme = [Windows.Forms.Button]::new(); $saveScheme.Text = '保存当前为方案'; $saveScheme.AutoSize = $true
$readAdvanced = [Windows.Forms.Button]::new(); $readAdvanced.Text = '读取当前值'; $readAdvanced.AutoSize = $true
$openPanel = [Windows.Forms.Button]::new(); $openPanel.Text = '启动游戏内面板'; $openPanel.AutoSize = $true
$actions2.Controls.Add($schemeLabel); $actions2.Controls.Add($schemeCombo); $actions2.Controls.Add($applyScheme)
$actions2.Controls.Add($schemeName); $actions2.Controls.Add($saveScheme); $actions2.Controls.Add($readAdvanced)
$actions2.Controls.Add($openPanel)
$script:lastWatchLog = $null

$openPanel.Add_Click({ Invoke-Ui {
    $panelExe = Join-Path $PSScriptRoot 'bin/dlss5-panel.exe'
    if (-not [IO.File]::Exists($panelExe)) { throw '面板程序缺失：bin/dlss5-panel.exe' }
    $running = @(Get-CimInstance Win32_Process -Filter "Name = 'dlss5-panel.exe'" -ErrorAction Stop)
    if ($running.Count -gt 0) { $output.Text = '面板已在运行。'; return }
    $logDir = Join-Path $project ('_scratch/dlss5-aio/panel-logs/' + [guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($logDir) | Out-Null
    Start-Process -FilePath $panelExe -ArgumentList @('--game', (Join-Path $clientRow.Box.Text 'wow.exe')) -WindowStyle Hidden -RedirectStandardOutput (Join-Path $logDir 'stdout.log') -RedirectStandardError (Join-Path $logDir 'stderr.log')
    $script:lastPanelLog = $logDir
    $output.Text = '游戏内面板已启动：收起态在游戏右上角，点它或按 Alt+F10 展开。日志：' + $logDir
} })

$output = [Windows.Forms.TextBox]::new()
$output.Dock = 'Fill'; $output.Multiline = $true; $output.ReadOnly = $true
$output.ScrollBars = 'Both'; $output.WordWrap = $false
$output.Font = [Drawing.Font]::new('Microsoft YaHei UI', 9)
$output.Text = "请选择隔离客户端，点击检查状态。`r`n独立输出候选需要同时启动鼠标助手。该助手不会启动游戏。`r`n游戏中可用 F10 对比整条处理管线与原画。"
$statusStrip = [Windows.Forms.Label]::new()
$statusStrip.Dock = 'Fill'
$statusStrip.Text = '状态：待刷新'
$statusStrip.Font = [Drawing.Font]::new('Microsoft YaHei UI', 9)
$statusStrip.ForeColor = [Drawing.Color]::FromArgb(255, 158, 255, 192)
$statusStrip.BackColor = [Drawing.Color]::FromArgb(255, 16, 30, 40)
$layout.Controls.Add($statusStrip, 0, 6)
$layout.Controls.Add($output, 0, 7)

function Invoke-Ui([scriptblock]$Action) {
    $form.UseWaitCursor = $true
    [Windows.Forms.Application]::DoEvents()
    try { & $Action }
    catch { $output.Text = '操作未完成：' + $_.Exception.Message }
    finally {
        $form.UseWaitCursor = $false
        try { Update-StatusStrip } catch { }
    }
}

function Update-Operations {
    $operations.Items.Clear()
    foreach ($item in @(Get-Dlss5Operations -Client $clientRow.Box.Text)) {
        $null = $operations.Items.Add($item)
    }
    if ($operations.Items.Count -eq 1) { $operations.SelectedIndex = 0 }
}

$check.Add_Click({ Invoke-Ui {
    $status = Get-Dlss5Status -Client $clientRow.Box.Text
    $lines = @("游戏文件：$($status.GameIdentity)",
        "游戏进程：$($status.GameProcesses)；载体进程：$($status.HostProcesses)；无法识别的同名进程：$($status.UnidentifiedProcesses)",
        "配置：AIO 管线=$($status.Config.PipelineEnabled)，NR=$($status.Config.NeuralRendering)，模型=$($status.Config.Model)，强度=$($status.Config.Intensity)，FG=$($status.Config.FrameGeneration)，Optical Flow=$($status.Config.NvidiaOpticalFlowMotion)，独立输出=$($status.Config.DetachedPresentation)，D3D9 CPU 桥=$($status.Config.ClassicD3D9CpuBridge)")
    $lines += @($status.GraphicsDevices | ForEach-Object { "系统显示设备：$($_.Name)，驱动：$($_.DriverVersion)" })
    $lines += '系统设备列表不代表载体实际使用的 GPU。'
    if ($status.Config.Error) { $lines += '配置读取问题：' + $status.Config.Error }
    if ($status.Telemetry.Available) {
        $lines += "载体遥测：NR 开关=$($status.Telemetry.NrEnabled)，处理管线就绪=$($status.Telemetry.PipelineReady)，NR 帧数=$($status.Telemetry.NrFrames)，已选处理输出=$($status.Telemetry.ProcessedOutputSelected)"
        $lines += '遥测为载体报告；最终画面与鼠标仍需游戏验收。'
    } else { $lines += '载体处理状态：未确认（' + $status.Telemetry.Reason + '）' }
    $lines += ''; $lines += @($status.Files | ForEach-Object { "$($_.Status)  $($_.Name)  $($_.Machine)  $($_.Version)" })
    $output.Text = $lines -join "`r`n"
    if ($null -ne $status.Config.NeuralRendering) { $nr.Checked = $status.Config.NeuralRendering -ne '0' }
    if ($status.Config.Model -in @('1', '2', '3')) { $model.SelectedIndex = [int]$status.Config.Model - 1 }
    if ($null -ne $status.Config.NvidiaOpticalFlowMotion) { $flow.Checked = $status.Config.NvidiaOpticalFlowMotion -eq '1' }
    if ($null -ne $status.Config.DetachedPresentation) { $detached.Checked = $status.Config.DetachedPresentation -eq '1' }
    $parsed = [double]0
    if ($null -ne $status.Config.Intensity -and [double]::TryParse($status.Config.Intensity,
        [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed) -and
        $parsed -ge 0 -and $parsed -le 2) { $intensity.Value = [decimal]$parsed }
    Update-Operations
} })

$preview.Add_Click({ Invoke-Ui {
    $result = Get-Dlss5InstallPreview -Client $clientRow.Box.Text -Stage $stageRow.Box.Text
    $lines = @('目标：' + $result.Client, '组件来源：' + $result.Stage,
        ('可安装：' + $(if ($result.CanInstall) { '是' } else { '否，请处理以下问题' })), '')
    $lines += @($result.Items | ForEach-Object { $(if ($_.Issue) { $_.Issue } elseif ($_.ReplacesOriginal) { '备份原件并替换' } else { '新增' }) + '  ' + $_.Target })
    $output.Text = $lines -join "`r`n"
} })

$install.Add_Click({ Invoke-Ui {
    $result = Invoke-Dlss5Install -Client $clientRow.Box.Text -Stage $stageRow.Box.Text
    $output.Text = "安装文件已回读：$($result.Count) 项。`r`n备份和事务记录：$($result.Operation)`r`n游戏效果尚未验收。"
    Update-Operations
} })

$save.Add_Click({ Invoke-Ui {
    $result = Set-Dlss5AdvancedSettings -Client $clientRow.Box.Text -NeuralRendering $nr.Checked `
        -Model ($model.SelectedIndex + 1) -Intensity ([double]$intensity.Value) `
        -LocalTone ([double]$tone.Value) -LocalStructure ([double]$structure.Value) -SkinStructure ([double]$skin.Value) `
        -DetachedPresentation $detached.Checked -OpticalFlow $flow.Checked
    $output.Text = "设置状态：$($result.Status)`r`n备份：$($result.Backup)`r`n修改在下次启动游戏时生效。"
} })

$reset.Add_Click({ Invoke-Ui {
    $result = Set-Dlss5Settings -Client $clientRow.Box.Text -NeuralRendering $true -Intensity 1 -Model 1 -DetachedPresentation $true -OpticalFlow $false
    $nr.Checked = $true; $model.SelectedIndex = 0; $intensity.Value = 1; $detached.Checked = $true; $flow.Checked = $false
    $output.Text = "候选默认设置状态：$($result.Status)`r`n备份：$($result.Backup)`r`n重启游戏后生效；独立输出需要同时启动鼠标助手。"
} })

$watch.Add_Click({ Invoke-Ui {
    $result = Start-Dlss5WindowWatch -Client $clientRow.Box.Text -Watcher $Watcher
    $script:lastWatchLog = $result.LogDirectory
    $output.Text = "$($result.Message)`r`n助手 PID：$($result.ProcessId)`r`n日志：$($result.LogDirectory)`r`n请自行启动游戏；助手空等 10 分钟或游戏退出后结束。"
} })

$watchLog.Add_Click({ Invoke-Ui {
    if (-not $script:lastWatchLog -or -not [IO.Directory]::Exists($script:lastWatchLog)) { throw '本次窗口尚未启动助手。' }
    $lines = @()
    foreach ($name in @('stdout.log', 'stderr.log')) {
        $path = Join-Path $script:lastWatchLog $name
        if ([IO.File]::Exists($path)) { $lines += @(Get-Content -LiteralPath $path -Tail 50 -Encoding UTF8 -ErrorAction Stop) }
    }
    $output.Text = if ($lines.Count) { $lines -join "`r`n" } else { '助手暂无日志。' }
} })
$panelLog.Add_Click({ Invoke-Ui {
    if (-not $script:lastPanelLog -or -not [IO.Directory]::Exists($script:lastPanelLog)) { throw '本次窗口尚未启动面板。' }
    $lines = @()
    foreach ($name in @('stdout.log', 'stderr.log')) {
        $path = Join-Path $script:lastPanelLog $name
        if ([IO.File]::Exists($path)) { $lines += @(Get-Content -LiteralPath $path -Tail 60 -Encoding UTF8 -ErrorAction Stop) }
    }
    $output.Text = if ($lines.Count) { $lines -join "`r`n" } else { '面板暂无日志。' }
} })

$pipeline.Add_Click({ Invoke-Ui {
    $status = Get-Dlss5Status -Client $clientRow.Box.Text
    if ($status.Config.PipelineEnabled -notin @('0', '1')) { throw '无法确定当前 AIO 管线开关。' }
    $enable = $status.Config.PipelineEnabled -eq '0'
    $result = Set-Dlss5Pipeline -Client $clientRow.Box.Text -Enabled $enable
    $name = if ($enable) { '启用' } else { '停用' }
    $output.Text = "已保存：$name AIO 管线。重启游戏后生效。`r`nReShade 仍会加载；恢复 DXVK 请使用恢复安装前文件。`r`n备份：$($result.Backup)"
} })

$diagnose.Add_Click({ Invoke-Ui {
    $dialog = [Windows.Forms.SaveFileDialog]::new()
    $dialog.Filter = 'JSON 文件 (*.json)|*.json'; $dialog.FileName = 'dlss5-diagnostic.json'
    $dialog.OverwritePrompt = $false
    try {
        if ($dialog.ShowDialog($form) -eq [Windows.Forms.DialogResult]::OK) {
            $path = Export-Dlss5Diagnostics -Client $clientRow.Box.Text -Destination $dialog.FileName
            $output.Text = "诊断已保存：$path`r`n未包含运行库、账号、聊天、截图或原始日志。"
        }
    } finally { $dialog.Dispose() }
} })

$repair.Add_Click({ Invoke-Ui {
    if ($operations.SelectedIndex -lt 0) { throw '请先检查状态并选择安装记录。' }
    $result = Invoke-Dlss5Repair -Operation ([string]$operations.SelectedItem.Operation)
    $output.Text = "修复状态：$($result.Status)`r`n修复文件：$($result.Repaired) 项`r`n保留的配置：$($result.Preserved -join '，')`r`n记录：$($result.Report)`r`n$($result.Errors -join "`r`n")"
} })

$restore.Add_Click({ Invoke-Ui {
    if ($operations.SelectedIndex -lt 0) { throw '请先检查状态并选择安装记录。' }
    $result = Invoke-Dlss5Restore -Operation ([string]$operations.SelectedItem.Operation)
    $output.Text = "恢复状态：$($result.Status)`r`n已保存的修改文件数：$($result.SavedModified)`r`n记录：$($result.Report)`r`n$($result.Errors -join "`r`n")"
    Update-Operations
} })

function Update-Schemes {
    $schemeCombo.Items.Clear()
    foreach ($s in @(Get-Dlss5Schemes)) { $null = $schemeCombo.Items.Add($s) }
    if ($schemeCombo.Items.Count -gt 0) { $schemeCombo.SelectedIndex = 0 }
}
function Read-AdvancedIntoUi {
    $a = Get-Dlss5AdvancedSettings -Client $clientRow.Box.Text
    if ($null -ne $a.NeuralRendering) { $nr.Checked = $a.NeuralRendering -ne '0' }
    if ($a.Model -in @('1','2','3')) { $model.SelectedIndex = [int]$a.Model - 1 }
    $parsed = [double]0
    foreach ($pair in @(@($intensity,$a.Intensity,0,2),@($tone,$a.LocalTone,0,2),@($structure,$a.LocalStructure,0,2),@($skin,$a.SkinStructure,-1,1))) {
        $box = $pair[0]; $raw = [string]$pair[1]
        if ($raw -and [double]::TryParse($raw, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) {
            $lo = [decimal]$pair[2]; $hi = [decimal]$pair[3]
            $dec = [decimal]$parsed
            if ($dec -lt $lo) { $dec = $lo } elseif ($dec -gt $hi) { $dec = $hi }
            $box.Value = $dec
        }
    }
}
$applyScheme.Add_Click({ Invoke-Ui {
    if ($schemeCombo.SelectedIndex -lt 0) { throw '请先选择档位。' }
    $picked = $schemeCombo.SelectedItem
    $result = Apply-Dlss5Scheme -Client $clientRow.Box.Text -Path $picked.Path
    Read-AdvancedIntoUi
    $output.Text = "已应用档位【$($picked.Name)】（$($picked.Source)）：$($result.Status)`r`n档位说明：$($picked.Description)`r`n备份：$($result.Backup)`r`n重启游戏后生效；独立输出需同时启动鼠标助手。"
} })
$saveScheme.Add_Click({ Invoke-Ui {
    if (-not $schemeName.Text.Trim()) { throw '请先在输入框填方案名（1-24 字符）。' }
    $path = Save-Dlss5Scheme -Client $clientRow.Box.Text -Name $schemeName.Text
    Update-Schemes
    $output.Text = "方案已保存：$path`r`n以后在档位下拉里选它即可应用。"
} })
$readAdvanced.Add_Click({ Invoke-Ui {
    Read-AdvancedIntoUi
    $output.Text = '已从当前配置读取档位微调值（含你在游戏内 Home → Add-ons 里改过的值）。'
} })

Update-Schemes
try { Read-AdvancedIntoUi } catch { }

function Update-StatusStrip {
    $clientFull = $clientRow.Box.Text
    $gameRunning = $false
    $watchPid = $null
    try {
        $gameExe = Join-Path $clientFull 'wow.exe'
        if ([IO.File]::Exists($gameExe)) {
            $gamePath = [IO.Path]::GetFullPath($gameExe)
            $gameProcs = @(Get-CimInstance Win32_Process -Filter "Name = 'wow.exe'" -ErrorAction Stop |
                Where-Object { $_.ExecutablePath -and $_.ExecutablePath.Equals($gamePath, [StringComparison]::OrdinalIgnoreCase) })
            $gameRunning = $gameProcs.Count -gt 0
        }
        $panelPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'bin/dlss5-panel.exe'))
        $panelProcs = @(Get-CimInstance Win32_Process -Filter "Name = 'dlss5-panel.exe'" -ErrorAction Stop |
            Where-Object { $_.ExecutablePath -and $_.ExecutablePath.Equals($panelPath, [StringComparison]::OrdinalIgnoreCase) })
        if ($panelProcs.Count -gt 0) { $watchPid = $panelProcs[0].ProcessId }
    } catch { }
    $parts = @()
    if ($null -ne $watchPid) { $parts += "面板：运行中 (PID $watchPid)" } else { $parts += "面板：未运行" }
    if ($gameRunning) { $parts += "游戏：运行中" } else { $parts += "游戏：未运行" }
    try {
        $a = Get-Dlss5AdvancedSettings -Client $clientFull
        $nrText = if ($a.NeuralRendering -eq '1') { '开' } elseif ($a.NeuralRendering -eq '0') { '关' } else { '未确认' }
        $schemeName = '自定义'
        $cur = $null
        try {
            $cur = @{ model = [int]$a.Model; intensity = [double]$a.Intensity; localTone = [double]$a.LocalTone;
                localStructure = [double]$a.LocalStructure; skinStructure = [double]$a.SkinStructure;
                neuralRendering = ($a.NeuralRendering -eq '1') }
        } catch { }
        if ($cur) {
            foreach ($s in @(Get-Dlss5Schemes)) {
                try {
                    $v = ([IO.File]::ReadAllText($s.Path, [Text.UTF8Encoding]::new($false, $true)) | ConvertFrom-Json).values
                    if ([int]$v.model -eq $cur.model -and [bool]$v.neuralRendering -eq $cur.neuralRendering -and
                        [math]::Abs([double]$v.intensity - $cur.intensity) -lt 0.005 -and
                        [math]::Abs([double]$v.localTone - $cur.localTone) -lt 0.005 -and
                        [math]::Abs([double]$v.localStructure - $cur.localStructure) -lt 0.005 -and
                        [math]::Abs([double]$v.skinStructure - $cur.skinStructure) -lt 0.005) {
                        $schemeName = $s.Name; break
                    }
                } catch { }
            }
        }
        $parts += "NR：$nrText"
        $parts += "模型：$($a.Model)"
        $parts += "档位：$schemeName"
        $parts += ("强度 {0} / 色调 {1} / 结构 {2} / 皮肤 {3}" -f $a.Intensity, $a.LocalTone, $a.LocalStructure, $a.SkinStructure)
    } catch {
        $parts += '配置：读取失败'
    }
    $statusStrip.Text = $parts -join '　|　'
}

# 鼠标助手自动守护：刷新状态条；游戏在跑/独立输出/助手未启 → 自动拉起，防"忘启动/超时自退"导致鼠标失效。
$watchTimer = [Windows.Forms.Timer]::new()
$watchTimer.Interval = 25000
$script:watchAutoNote = ''
$watchTimer.Add_Tick({
    try { Update-StatusStrip } catch { return }
    try {
        if ($statusStrip.Text -notmatch '游戏：运行中') { return }
        $panelProcs = @(Get-CimInstance Win32_Process -Filter "Name = 'dlss5-panel.exe'" -ErrorAction Stop)
        if ($panelProcs.Count -gt 0) { return }
        $panelExe = Join-Path $PSScriptRoot 'bin/dlss5-panel.exe'
        if (-not [IO.File]::Exists($panelExe)) { return }
        $logDir = Join-Path $project ('_scratch/dlss5-aio/panel-logs/' + [guid]::NewGuid().ToString('N'))
        [IO.Directory]::CreateDirectory($logDir) | Out-Null
        Start-Process -FilePath $panelExe -ArgumentList @('--game', (Join-Path $clientRow.Box.Text 'wow.exe')) -WindowStyle Hidden -RedirectStandardOutput (Join-Path $logDir 'stdout.log') -RedirectStandardError (Join-Path $logDir 'stderr.log')
        $script:lastPanelLog = $logDir
        $script:watchAutoNote = ''
        $output.AppendText("[" + (Get-Date -Format 'HH:mm:ss') + "] 检测到游戏运行但面板未启动，已自动拉起。`r`n")
    } catch {
        $msg = $_.Exception.Message
        if ($script:watchAutoNote -ne $msg) {
            $script:watchAutoNote = $msg
            $output.AppendText("[" + (Get-Date -Format 'HH:mm:ss') + "] 助手自动拉起未执行：" + $msg + "`r`n")
        }
    }
})
$watchTimer.Start()
try { Update-StatusStrip } catch { }

$null = $form.ShowDialog()
$form.Dispose()
