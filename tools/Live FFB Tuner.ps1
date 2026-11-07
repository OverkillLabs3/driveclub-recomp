# Live FFB Tuner: the force feedback settings of the launcher's Wheel page (the wheel_ffb_* lines of
# data\user\input_config\default.ini) in a small window that stays open over the game. Every change is saved after a
# short pause; a running game picks it up within a second, so it can be felt on the next corner.
# Started by "Live FFB Tuner.bat" next to it, or the launcher's Open Live FFB Tuner button.
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$ini = Join-Path $PSScriptRoot 'data\user\input_config\default.ini'
$detected = Join-Path $PSScriptRoot 'data\user\input_config\wheel_detected.txt'
if (-not (Test-Path -LiteralPath $ini)) {
    [System.Windows.Forms.MessageBox]::Show("Settings file not found:`n$ini`n`nStart the game once first.", 'Live FFB Tuner') | Out-Null
    exit 1
}

# The settings that suit each kind of wheelbase, as the game uses them where default.ini has no line (the runtime's
# FFB_WHEEL_DEFAULTS, src/runtime_wheel.inc; the launcher's kWheelKind). Choosing a wheel type sets all of them.
$kinds = @{
    direct = @{ wheel_ffb_physics_detail=75;  wheel_ffb_physics_damper=0; wheel_ffb_physics_min_force=0; wheel_ffb_physics_friction=6
                wheel_ffb_physics_countersteer='off'; wheel_ffb_physics_power_steering='off' }
    belt   = @{ wheel_ffb_physics_detail=100; wheel_ffb_physics_damper=0; wheel_ffb_physics_min_force=0; wheel_ffb_physics_friction=2
                wheel_ffb_physics_countersteer='on';  wheel_ffb_physics_power_steering='on' }
    gear   = @{ wheel_ffb_physics_detail=60;  wheel_ffb_physics_damper=0; wheel_ffb_physics_min_force=5; wheel_ffb_physics_friction=0
                wheel_ffb_physics_countersteer='on';  wheel_ffb_physics_power_steering='on' }
}
# What a wheel type sets besides those: the same for every kind.
$common = [ordered]@{ wheel_ffb_model='physics'; wheel_ffb_physics_gain=80; wheel_ffb_physics_max_force=100; wheel_ffb_physics_filter=0
                      wheel_ffb_physics_effects=0; wheel_ffb_physics_hit_protection='off' }

# Key, label, kind, range or choices, default (a kind's own where it has one), help (a tooltip, and under the sliders).
$settings = @(
    @{ Key='wheel_ffb_wheel_type'; Label='Wheel type'; Kind='choice'; Choices=@('auto','direct','belt','gear'); Default='auto'
       Help='Choosing one sets every setting to what suits that kind of wheelbase: direct drive (the motor turns the shaft: Fanatec DD, Moza, Simagic, Simucube...), belt (Thrustmaster T300, Fanatec CSL Elite...), gear (Logitech G29/G920/G923, Thrustmaster T150...). auto: as the game found it from the wheel''s name.' }
    @{ Key='wheel_ffb_physics_countersteer'; Label='Countersteer assist'; Kind='choice'; Choices=@('on','off'); Default='off'
       Help='on: the game turns the road wheels further than your wheel when you countersteer a slide (up to a third more). off: the road wheels follow only your wheel, so you catch the slides yourself, as in a real car. Off on direct drive; on helps belt and gear wheels. This changes how the car handles, not only the force feedback.' }
    @{ Key='wheel_ffb_physics_gain';   Label='Gain %';      Kind='slider'; Min=0;  Max=300; Default=80
       Help='The whole steering force, for every car. Too strong: the wheel hits its limit in fast corners and feels flat. One car stronger or weaker than the rest: the FFB gain + and - buttons change only the car you are driving.' }
    @{ Key='wheel_ffb_physics_detail'; Label='Detail %';    Kind='slider'; Min=0;  Max=200; Default=75
       Help='How much of the quick changes in the force you feel: kerbs, bumps, the tyres biting and letting go. Raise it if the wheel feels dull; lower it if it feels busy or rattles.' }
    @{ Key='wheel_ffb_physics_damper'; Label='Damper %';    Kind='slider'; Min=0;  Max=100; Default=0
       Help='Heavier to turn the faster you turn, so the wheel stays calm and does not swing past the centre. Raise it if the centre feels light or vague. 0 if your wheelbase has a damper of its own (Fanatec NDP, Moza and Simagic damping).' }
    @{ Key='wheel_ffb_physics_max_force'; Label='Max force %'; Kind='slider'; Min=30; Max=100; Default=100
       Help='The most of the wheel''s force the steering asks for; stronger forces bend smoothly toward it. Try 70 to 85 if slides or hits kick back too hard. 100: no extra limit.' }
)

function Read-Values($path) {
    $v = @{}
    if (-not (Test-Path -LiteralPath $path)) { return $v }
    foreach ($line in [IO.File]::ReadAllLines($path)) {
        if ($line -match '^\s*([\w]+)\s*=\s*([^#]*)') { $v[$Matches[1].ToLower()] = $Matches[2].Trim() }
    }
    return $v
}
function Write-Values($values) {
    $text = [IO.File]::ReadAllText($ini)
    $nl = "`n"; if ($text -match "`r`n") { $nl = "`r`n" }
    foreach ($key in $values.Keys) {
        $line = "$key = $($values[$key])"
        $rx = New-Object Text.RegularExpressions.Regex ("(?im)^[ \t]*" + [Regex]::Escape($key) + "[ \t]*=[^\r\n]*")
        if ($rx.IsMatch($text)) { $text = $rx.Replace($text, $line.Replace('$', '$$'), 1) }
        else {
            $last = [Regex]::Matches($text, '(?im)^[ \t]*wheel_ffb\w*[ \t]*=[^\r\n]*')
            if ($last.Count) { $m = $last[$last.Count - 1]; $text = $text.Insert($m.Index + $m.Length, $nl + $line) }
            else { $text = $text.TrimEnd() + $nl + $line + $nl }
        }
    }
    [IO.File]::WriteAllText($ini, $text, (New-Object Text.UTF8Encoding($false)))
}
# The kind whose settings apply: the file's wheel type, or on auto the game's find (belt before it has seen a wheel).
function Kind-Of($values, $type) {
    if ($type -in @('direct', 'belt', 'gear')) { return $type }
    $found = (Read-Values $detected)['type']
    if ($found -in @('direct', 'belt', 'gear')) { return $found }
    return 'belt'
}
# A setting's value where the file has no line: its kind's, else its own default.
function Default-Of($key, $values) {
    $kind = $kinds[(Kind-Of $values $values['wheel_ffb_wheel_type'])]
    if ($kind.ContainsKey($key)) { return [string]$kind[$key] }
    return [string]($settings | Where-Object { $_.Key -eq $key }).Default
}

$loaded = Read-Values $ini
$script:backedUp = $false
$script:syncing = $false
$script:typeSet = $null   # a wheel type chosen here and not saved yet: its settings the window does not show go with it
$script:shown = @{}   # each setting's value as the file has it (as shown when it was read or written)
$script:seen = [IO.File]::GetLastWriteTimeUtc($ini)
$controls = @{}

$form = New-Object Windows.Forms.Form
$form.Text = 'Live FFB Tuner - DriveClub'
$form.AutoSize = $true; $form.AutoSizeMode = 'GrowAndShrink'
$form.FormBorderStyle = 'FixedSingle'; $form.MaximizeBox = $false
$form.StartPosition = 'CenterScreen'
$form.Font = New-Object Drawing.Font('Segoe UI', 9)
$form.Padding = New-Object Windows.Forms.Padding(10)
$tip = New-Object Windows.Forms.ToolTip
$tip.AutoPopDelay = 15000

$table = New-Object Windows.Forms.TableLayoutPanel
$table.AutoSize = $true; $table.ColumnCount = 3; $table.Dock = 'Fill'
[void]$table.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle('AutoSize')))
[void]$table.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle('Absolute', 300)))
[void]$table.ColumnStyles.Add((New-Object Windows.Forms.ColumnStyle('AutoSize')))

$help = New-Object Windows.Forms.Label
$help.AutoSize = $false; $help.Width = 520; $help.Height = 76; $help.ForeColor = [Drawing.Color]::DimGray
$status = New-Object Windows.Forms.Label
$status.AutoSize = $true; $status.Text = 'Changes are saved as you make them. Not driving right now? They apply the next time the game starts.'

$timer = New-Object Windows.Forms.Timer
$timer.Interval = 400
function Mark-Changed { $timer.Stop(); $timer.Start(); $status.Text = 'Saving...' }

function Value-Of($key) {
    $c = $controls[$key]
    switch ($c.Kind) {
        'slider' { return [string][int]$c.Box.Value }
        'choice' { return [string]$c.Box.SelectedItem }
    }
}
# The lines of the settings changed here since the file was last read or written (only those are written, so what the
# launcher changed meanwhile in other lines stays); a wheel type chosen here brings its other settings along.
function Collect-Values {
    $values = [ordered]@{}
    foreach ($s in $settings) {
        $v = Value-Of $s.Key
        if ($v -ceq $script:shown[$s.Key]) { continue }
        $values[$s.Key] = $v
    }
    if ($script:typeSet) {
        $kind = $kinds[$script:typeSet]
        foreach ($k in $common.Keys) { if (-not $values.Contains($k) -and -not $controls.ContainsKey($k)) { $values[$k] = [string]$common[$k] } }
        foreach ($k in $kind.Keys) { if (-not $values.Contains($k) -and -not $controls.ContainsKey($k)) { $values[$k] = [string]$kind[$k] } }
    }
    return $values
}
# Writes those lines; from then on they are what the file has.
function Save-Values($values) {
    Write-Values $values
    foreach ($k in @($values.Keys)) { if ($controls.ContainsKey($k)) { $script:shown[$k] = $values[$k] } }
    $script:typeSet = $null
}
# The launcher (or an editor) changed the file: its values are shown here, without being saved back. A setting changed
# here and not saved yet keeps its value. True when something changed.
function Sync-FromDisk {
    $script:seen = [IO.File]::GetLastWriteTimeUtc($ini)
    $now = Read-Values $ini
    $any = $false
    foreach ($s in $settings) {
        $v = $now[$s.Key]
        if ($null -eq $v) { $v = Default-Of $s.Key $now }
        if ([string]$v -ceq [string]$script:shown[$s.Key]) { continue }
        if ((Value-Of $s.Key) -cne $script:shown[$s.Key]) { continue }
        Show-Value $s.Key $v
        $script:shown[$s.Key] = Value-Of $s.Key
        $any = $true
    }
    Update-Model $now
    return $any
}
function Show-Value($key, $value) {
    $c = $controls[$key]; $s = $c.Setting
    $script:syncing = $true
    switch ($c.Kind) {
        'slider' {
            $n = 0
            if (-not [int]::TryParse([string]$value, [ref]$n)) { $n = [int]$s.Default }
            $n = [Math]::Max($s.Min, [Math]::Min($s.Max, $n))
            $c.Box.Value = $n; $c.Bar.Value = $n
        }
        'choice' {
            $i = $c.Box.Items.IndexOf(([string]$value).ToLower())
            if ($i -lt 0 -and $key -eq 'wheel_ffb_wheel_type') { $i = 0 }   # anything else reads as auto, as in the game
            if ($i -lt 0 -and $value) { $i = $c.Box.Items.Add([string]$value) }
            if ($i -lt 0) { $i = $c.Box.Items.IndexOf([string]$s.Default) }
            $c.Box.SelectedIndex = $i
        }
    }
    $script:syncing = $false
}
# A wheel type chosen here: every setting the window shows to what suits that kind (the others when it saves).
function Apply-Type($type) {
    $kindName = Kind-Of @{} $type
    $kind = $kinds[$kindName]
    foreach ($s in $settings) {
        if ($s.Key -eq 'wheel_ffb_wheel_type') { continue }
        if ($kind.ContainsKey($s.Key)) { Show-Value $s.Key $kind[$s.Key] }
        elseif ($common.Contains($s.Key)) { Show-Value $s.Key $common[$s.Key] }
    }
    $script:typeSet = $kindName
    $shown = @{ direct = 'direct drive'; belt = 'belt'; gear = 'gear' }[$kindName]
    $help.Text = "Set for a $shown wheel: gain 80, and the detail, damper and steering help that suit it. Drive a few corners, then fine-tune."
}

# The point of this window: tune while driving. Said up front, where it cannot be missed.
$live = New-Object Windows.Forms.Label
$live.AutoSize = $false; $live.Width = 530; $live.Height = 46
$live.BackColor = [Drawing.Color]::FromArgb(220, 236, 255); $live.ForeColor = [Drawing.Color]::FromArgb(20, 60, 120)
$live.Font = New-Object Drawing.Font('Segoe UI', 9, [Drawing.FontStyle]::Bold)
$live.Padding = New-Object Windows.Forms.Padding(8, 4, 8, 4)
$live.Margin = New-Object Windows.Forms.Padding(0, 0, 0, 10)
$live.Text = 'Changes apply LIVE: keep DriveClub running, drive, and move a slider. The wheel feels the change within a second, no restart needed.'
$table.Controls.Add($live, 0, 0); $table.SetColumnSpan($live, 3)
# The settings are Reworked Physics': on another model, said here (the launcher's Wheel page switches it back).
$model = New-Object Windows.Forms.Label
$model.AutoSize = $false; $model.Width = 530; $model.Height = 36; $model.ForeColor = [Drawing.Color]::DarkRed
$model.Text = 'Force feedback is on the Original model, which these settings do not change. Choose Reworked Physics on the launcher''s Wheel page.'
$table.Controls.Add($model, 0, 1); $table.SetColumnSpan($model, 3)
function Update-Model($values) { $model.Visible = $values['wheel_ffb_model'] -in @('original', 'software') }
$row = 2
foreach ($s in $settings) {
    $label = New-Object Windows.Forms.Label
    $label.Text = $s.Label; $label.AutoSize = $true; $label.Anchor = 'Left'
    $label.Margin = New-Object Windows.Forms.Padding(0, 6, 12, 0)
    $tip.SetToolTip($label, $s.Help)
    $table.Controls.Add($label, 0, $row)
    $entry = @{ Kind = $s.Kind; Setting = $s; Label = $label }
    switch ($s.Kind) {
        'slider' {
            $bar = New-Object Windows.Forms.TrackBar
            $bar.Minimum = $s.Min; $bar.Maximum = $s.Max; $bar.Width = 290; $bar.TickStyle = 'None'
            $bar.AutoSize = $false; $bar.Height = 28
            $bar.SmallChange = 1; $bar.LargeChange = [Math]::Max(1, [int](($s.Max - $s.Min) / 20))
            $bar.Tag = $s.Key
            $box = New-Object Windows.Forms.NumericUpDown
            $box.Minimum = $s.Min; $box.Maximum = $s.Max; $box.Width = 70; $box.Tag = $s.Key
            $box.Anchor = 'Left'
            $bar.Add_Scroll({ if (-not $script:syncing) { $script:syncing = $true; $controls[$this.Tag].Box.Value = $this.Value; $script:syncing = $false; Mark-Changed } })
            $box.Add_ValueChanged({ if (-not $script:syncing) { $script:syncing = $true; $controls[$this.Tag].Bar.Value = [int]$this.Value; $script:syncing = $false; Mark-Changed } })
            $bar.Add_Enter({ $help.Text = $controls[$this.Tag].Setting.Help })
            $box.Add_Enter({ $help.Text = $controls[$this.Tag].Setting.Help })
            $table.Controls.Add($bar, 1, $row); $table.Controls.Add($box, 2, $row)
            $entry.Bar = $bar; $entry.Box = $box
        }
        'choice' {
            $box = New-Object Windows.Forms.ComboBox
            $box.DropDownStyle = 'DropDownList'; $box.Width = 140; $box.Tag = $s.Key
            foreach ($c in $s.Choices) { [void]$box.Items.Add($c) }
            $box.Add_SelectedIndexChanged({
                if ($script:syncing) { return }
                if ($this.Tag -eq 'wheel_ffb_wheel_type') { Apply-Type ([string]$this.SelectedItem) }
                Mark-Changed
            })
            $box.Add_Enter({ $help.Text = $controls[$this.Tag].Setting.Help })
            $box.Margin = New-Object Windows.Forms.Padding(3, 5, 3, 5)
            $table.Controls.Add($box, 1, $row)
            $entry.Box = $box
        }
    }
    $controls[$s.Key] = $entry
    $value = $loaded[$s.Key]
    if ($null -eq $value) { $value = Default-Of $s.Key $loaded }
    Show-Value $s.Key $value
    $script:shown[$s.Key] = Value-Of $s.Key
    $row++
}
Update-Model $loaded

$timer.Add_Tick({
    $timer.Stop()
    try {
        $values = Collect-Values
        if ($values.Count) {
            if (-not $script:backedUp) { Copy-Item -LiteralPath $ini -Destination "$ini.tuner-backup" -Force; $script:backedUp = $true }
            Save-Values $values
        }
        [void](Sync-FromDisk)   # what the launcher wrote meanwhile into other lines
        $status.Text = 'Saved ' + (Get-Date -Format 'HH:mm:ss') + '. Felt on the wheel within a second if the game is running.'
    } catch {
        $status.Text = 'Could not save: ' + $_.Exception.Message
    }
})
# Changes the launcher makes while this window is open show up here within half a second.
$watch = New-Object Windows.Forms.Timer
$watch.Interval = 500
$watch.Add_Tick({
    try {
        if ($timer.Enabled -or [IO.File]::GetLastWriteTimeUtc($ini) -eq $script:seen) { return }   # a save here is pending: after it
        if (Sync-FromDisk) { $status.Text = 'Updated from the launcher at ' + (Get-Date -Format 'HH:mm:ss') + '.' }
    } catch { }
})
$watch.Start()

$buttons = New-Object Windows.Forms.FlowLayoutPanel
$buttons.AutoSize = $true; $buttons.FlowDirection = 'LeftToRight'
$undo = New-Object Windows.Forms.Button
$undo.Text = 'Undo all changes'; $undo.AutoSize = $true
$undo.Add_Click({
    foreach ($s in $settings) { $v = $loaded[$s.Key]; if ($null -eq $v) { $v = Default-Of $s.Key $loaded }; Show-Value $s.Key $v }
    $script:typeSet = $null
    Mark-Changed
})
$close = New-Object Windows.Forms.Button
$close.Text = 'Close'; $close.AutoSize = $true
$close.Add_Click({ $form.Close() })
$buttons.Controls.Add($undo); $buttons.Controls.Add($close)

$table.Controls.Add($help, 0, $row); $table.SetColumnSpan($help, 3); $row++
$table.Controls.Add($status, 0, $row); $table.SetColumnSpan($status, 3); $row++
$table.Controls.Add($buttons, 0, $row); $table.SetColumnSpan($buttons, 3)
$form.Controls.Add($table)
# a save still pending when the window closes is made first
$form.Add_FormClosing({
    $watch.Stop()
    if ($timer.Enabled) { $timer.Stop(); $values = Collect-Values; if ($values.Count) { Save-Values $values } }
})
[void]$form.ShowDialog()
