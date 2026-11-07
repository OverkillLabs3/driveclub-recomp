# FFB Tuner: sliders for the wheel_ffb_* lines of data\user\input_config\default.ini.
# Every change is saved after a short pause; a running game picks the new values up within a second.
# Started by "FFB Tuner.bat" next to it.
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$ini = Join-Path $PSScriptRoot 'data\user\input_config\default.ini'
if (-not (Test-Path -LiteralPath $ini)) {
    [System.Windows.Forms.MessageBox]::Show("Settings file not found:`n$ini`n`nStart the game once first.", 'FFB Tuner') | Out-Null
    exit 1
}

# Key, label, kind, range or choices, default, help (shown as a tooltip and under the sliders). Group: physics (shown
# with the physics model only), classic (with the software and original models only); no group: always shown.
$settings = @(
    @{ Key='wheel_ffb_model';          Label='Force model';                Kind='choice'; Choices=@('physics','original'); Default='physics'
       Help='physics: the force comes from the car''s front tyres in the game''s simulation, as in a sim (Assetto Corsa style settings). original: the PS4 game''s own forces, a light spring.' }
    @{ Key='wheel_ffb_physics_gain';   Label='Gain %';                     Kind='slider'; Min=0; Max=300;  Default=100; Group='physics'
       Help='The whole steering force. 100: a 1 g corner at about 60% of the wheel''s maximum. Keep the log''s "FFB clipping" lines low.' }
    @{ Key='wheel_ffb_physics_detail'; Label='Detail %';                   Kind='slider'; Min=0; Max=200;  Default=100; Group='physics'
       Help='How much of the quick changes in the force you feel, on top of the steady force. Raise it if the wheel feels dead; lower it if it feels busy.' }
    @{ Key='wheel_ffb_physics_filter'; Label='Filter %';                   Kind='slider'; Min=0; Max=100;  Default=0;   Group='physics'
       Help='Smooths the force (100: 40 ms). 0 on a direct drive for every detail; raise it if the wheel feels grainy.' }
    @{ Key='wheel_ffb_physics_min_force'; Label='Minimum force %';         Kind='slider'; Min=0; Max=30;   Default=0;   Group='physics'
       Help='Lifts the smallest forces, for wheels that cannot make small forces (gear and belt wheels). 0 on a direct drive.' }
    @{ Key='wheel_ffb_physics_max_force'; Label='Maximum force %';         Kind='slider'; Min=30; Max=100; Default=100; Group='physics'
       Help='The most of the wheel''s force the steering asks for; stronger forces bend smoothly toward it, so a snap-back in a slide does not tower over the rest of the driving. Smaller forces stay as they are. 100: no extra limit.' }
    @{ Key='wheel_ffb_physics_damper'; Label='Damper %';                   Kind='slider'; Min=0; Max=100;  Default=5;   Group='physics'
       Help='Resistance to turning the wheel quickly. Steadies a direct drive. The same at every speed, so start low.' }
    @{ Key='wheel_ffb_physics_effects'; Label='Tyre effects %';            Kind='slider'; Min=0; Max=100;  Default=100; Group='physics'
       Help='A light buzz when a tyre slides past its grip: a front sliding or locking, the rear spinning or sliding; faster the faster it slides. 0: off (only the steering force). Lower it if it is too strong on your wheel.' }
    @{ Key='wheel_ffb_physics_hit_protection'; Label='Hit protection'; Kind='choice'; Choices=@('on','off'); Default='on'; Group='physics'
       Help='on: when the car hits a wall or another car, the force is held down for about half a second, so the hit cannot throw the wheel. off: every hit at full force, so keep your thumbs outside the rim. Can be changed while driving.' }
    @{ Key='wheel_ffb_physics_countersteer'; Label='Automatic countersteer'; Kind='choice'; Choices=@('on','off'); Default='on'; Group='physics'
       Help='on: as the game, which turns the road wheels further than your wheel when you countersteer (up to a third with wheel assist off), and passes less past a steering limit. off: the road wheels follow only your wheel, so you catch the slides yourself. This changes how the car handles, not only the force feedback. Can be changed while driving.' }
    @{ Key='wheel_ffb_soft_lock';      Label='Soft lock';                  Kind='choice'; Choices=@('game','off','540','720','900','1080'); Default='game'
       Help='Stops the wheel at the car''s steering lock. game: the lock the game asks for.' }
    @{ Key='wheel_ffb_strength';       Label='Steering strength %';        Kind='slider'; Min=0; Max=500;  Default=100; Group='classic'
       Help='Overall steering force (software model). Keep the log''s "FFB clipping" lines low.' }
    @{ Key='wheel_ffb_center';         Label='Centre weight %';            Kind='slider'; Min=0; Max=100;  Default=0; Group='classic'
       Help='More force in the first degrees off centre, for a less dull centre. 0: in proportion, as the game.' }
    @{ Key='wheel_ffb_slide';          Label='Slide lightness %';          Kind='slider'; Min=0; Max=100;  Default=0; Group='classic'
       Help='Follows the game''s grip: when the tyres slide, the wheel gets lighter than the game''s grip alone makes it. Higher: lighter drifts.' }
    @{ Key='wheel_ffb_falloff';        Label='Falloff past grip %';        Kind='slider'; Min=0; Max=100;  Default=40; Group='classic'
       Help='Follows how far you steer past where the tyres want the wheel (big countersteer, sawing): the force drops off there. 0 holds it.' }
    @{ Key='wheel_ffb_smoothing';      Label='Smoothing ms';               Kind='slider'; Min=0; Max=50;   Default=8; Group='classic'
       Help='Higher: smoother, slightly slower. Lower: sharper, maybe grainy.' }
    @{ Key='wheel_ffb_spring';         Label='Centring stiffness %';       Kind='slider'; Min=0; Max=1000; Default=250; Group='classic'
       Help='How quickly the force builds as the wheel moves off where the tyres want it.' }
    @{ Key='wheel_ffb_spring_limit';   Label='Grip force limit %';         Kind='slider'; Min=5; Max=100;  Default=100; Group='classic'
       Help='The most the grip force can be, percent of the wheel''s maximum.' }
    @{ Key='wheel_ffb_damper';         Label='Damper %';                   Kind='slider'; Min=0; Max=300;  Default=30; Group='classic'
       Help='Resistance to turning at all. Lower: lighter and more detailed. Too low can oscillate.' }
    @{ Key='wheel_ffb_vibration_limit'; Label='Vibration limit %';         Kind='slider'; Min=0; Max=50;   Default=10; Group='classic'
       Help='The strongest any vibration can be, percent of the wheel''s maximum torque. Kerbs and road texture are shares of this.' }
    @{ Key='wheel_ffb_vibration_response'; Label='Vibration response %';   Kind='slider'; Min=20; Max=100; Default=100; Group='classic'
       Help='100: in proportion to the game. Lower lifts the small vibrations (slow gravel, light texture) while the strongest stays at the limit.' }
    @{ Key='wheel_ffb_kerbs';          Label='Kerbs % of limit';           Kind='slider'; Min=0; Max=100;  Default=100; Group='classic'
       Help='Kerbs and rumble strips (also wheelspin): 100 = the vibration limit at the game''s strongest kerb. Test on a circuit with kerbs.' }
    @{ Key='wheel_ffb_texture';        Label='Road texture % of limit';    Kind='slider'; Min=0; Max=100;  Default=50; Group='classic'
       Help='Road surface: 100 = the vibration limit on the roughest surface.' }
    @{ Key='wheel_ffb_texture_mode';   Label='Road texture mode';          Kind='choice'; Choices=@('changes','full'); Default='changes'; Group='classic'
       Help='changes: only rough patches and surface changes. full: the steady hum as well.' }
    @{ Key='wheel_ffb_texture_floor';  Label='Texture floor % (changes)';  Kind='slider'; Min=0; Max=100;  Default=0; Group='classic'
       Help='In the changes mode: how much of a steady surface (gravel) stays. 0 removes it.' }
    @{ Key='wheel_ffb_long_rumble';    Label='Long rumble kept %';         Kind='slider'; Min=0; Max=100;  Default=25; Group='classic'
       Help='Wheelspin and burnouts: the kerb vibration fades to this after half a second, so the wheel does not shake itself loose. Short kerbs keep their full strength.' }
    @{ Key='wheel_ffb_vibration_wave'; Label='Vibration wave';             Kind='choice'; Choices=@('sine','square'); Default='square'; Group='classic'
       Help='sine: smooth texture. square: harsher, as the game asks. (The physics model has no vibrations: the road comes at its full strength.)' }
    @{ Key='wheel_ffb_invert';         Label='Invert forces';              Kind='check';  Default='false'
       Help='Only if the wheel pulls the wrong way.' }
)

function Read-Values {
    $v = @{}
    foreach ($line in [IO.File]::ReadAllLines($ini)) {
        if ($line -match '^\s*(wheel_ffb\w*)\s*=\s*([^#]*)') { $v[$Matches[1].ToLower()] = $Matches[2].Trim() }
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

$loaded = Read-Values
# "game" was the original model's name until 0.4.1, the launcher's default then: the game reads it as physics now
if ($loaded['wheel_ffb_model'] -eq 'game') { $loaded['wheel_ffb_model'] = 'physics' }
$script:backedUp = $false
$script:syncing = $false
$script:shown = @{}   # each setting's value as the file has it (as shown when it was read or written)
$script:seen = [IO.File]::GetLastWriteTimeUtc($ini)
$controls = @{}

$form = New-Object Windows.Forms.Form
$form.Text = 'FFB Tuner - DriveClub'
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
$help.AutoSize = $false; $help.Width = 520; $help.Height = 62; $help.ForeColor = [Drawing.Color]::DimGray
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
        'check'  { if ($c.Box.Checked) { return 'true' } else { return 'false' } }
    }
}
# The lines of the settings changed here since the file was last read or written (only those are written, so what the
# launcher changed meanwhile in other lines stays); a slider with Also writes its value to that key as well.
function Collect-Values {
    $values = [ordered]@{}
    foreach ($s in $settings) {
        $v = Value-Of $s.Key
        if ($v -ceq $script:shown[$s.Key]) { continue }
        $values[$s.Key] = $v
        if ($s.Also) { $values[$s.Also] = $v }
    }
    return $values
}
# Writes those lines; from then on they are what the file has.
function Save-Values($values) {
    Write-Values $values
    foreach ($k in @($values.Keys)) { $script:shown[$k] = $values[$k] }
}
# The launcher (or an editor) changed the file: its values are shown here, without being saved back. A setting changed
# here and not saved yet keeps its value. True when something changed.
function Sync-FromDisk {
    $script:seen = [IO.File]::GetLastWriteTimeUtc($ini)
    $now = Read-Values
    if ($now['wheel_ffb_model'] -eq 'game') { $now['wheel_ffb_model'] = 'physics' }
    $any = $false
    foreach ($s in $settings) {
        $v = $now[$s.Key]
        if ($null -eq $v) { $v = $s.Default }
        if ([string]$v -ceq [string]$script:shown[$s.Key]) { continue }
        if ((Value-Of $s.Key) -cne $script:shown[$s.Key]) { continue }
        Show-Value $s.Key $v
        $script:shown[$s.Key] = Value-Of $s.Key
        $any = $true
    }
    if ($any) { Update-Groups }
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
            $i = $c.Box.Items.IndexOf([string]$value)
            if ($i -lt 0 -and $value) { $i = $c.Box.Items.Add([string]$value) }
            if ($i -lt 0) { $i = $c.Box.Items.IndexOf([string]$s.Default) }
            $c.Box.SelectedIndex = $i
        }
        'check' { $c.Box.Checked = ([string]$value).ToLower() -eq 'true' }
    }
    $script:syncing = $false
}

# Presets: starting points that set every slider; fine-tune from there.
$presets = [ordered]@{
    'Physics model, direct drive (sim style)' = @{
        wheel_ffb_model='physics'; wheel_ffb_physics_gain=100; wheel_ffb_physics_filter=0; wheel_ffb_physics_min_force=0
        wheel_ffb_physics_damper=5; wheel_ffb_physics_detail=100; wheel_ffb_physics_max_force=100; wheel_ffb_soft_lock='game' }
    'Physics model, gear or belt wheel - untested' = @{
        wheel_ffb_model='physics'; wheel_ffb_physics_gain=130; wheel_ffb_physics_filter=10; wheel_ffb_physics_min_force=8
        wheel_ffb_physics_damper=0; wheel_ffb_physics_detail=100; wheel_ffb_physics_max_force=100; wheel_ffb_soft_lock='game' }
    'Direct drive (8 Nm Fanatec baseline)' = @{
        wheel_ffb_model='software'; wheel_ffb_strength=200; wheel_ffb_center=100; wheel_ffb_slide=2; wheel_ffb_falloff=5
        wheel_ffb_smoothing=0; wheel_ffb_soft_lock='game'; wheel_ffb_spring=90; wheel_ffb_spring_limit=100; wheel_ffb_damper=10
        wheel_ffb_vibration_limit=10; wheel_ffb_vibration_response=46; wheel_ffb_kerbs=100; wheel_ffb_texture=50
        wheel_ffb_texture_mode='changes'; wheel_ffb_texture_floor=75; wheel_ffb_long_rumble=25; wheel_ffb_vibration_wave='sine'
        wheel_ffb_invert='false' }
    'Gear or belt driven (Logitech, Thrustmaster) - untested' = @{
        wheel_ffb_model='software'; wheel_ffb_strength=225; wheel_ffb_center=100; wheel_ffb_slide=2; wheel_ffb_falloff=5
        wheel_ffb_smoothing=8; wheel_ffb_soft_lock='game'; wheel_ffb_spring=90; wheel_ffb_spring_limit=100; wheel_ffb_damper=0
        wheel_ffb_vibration_limit=25; wheel_ffb_vibration_response=46; wheel_ffb_kerbs=100; wheel_ffb_texture=50
        wheel_ffb_texture_mode='changes'; wheel_ffb_texture_floor=75; wheel_ffb_long_rumble=25; wheel_ffb_vibration_wave='sine'
        wheel_ffb_invert='false' }
}
$presetLabel = New-Object Windows.Forms.Label
$presetLabel.Text = 'Preset'; $presetLabel.AutoSize = $true; $presetLabel.Anchor = 'Left'
$presetLabel.Margin = New-Object Windows.Forms.Padding(0, 6, 12, 0)
$presetBox = New-Object Windows.Forms.ComboBox
$presetBox.DropDownStyle = 'DropDownList'; $presetBox.Width = 290
[void]$presetBox.Items.Add('Choose a starting point...')
foreach ($name in $presets.Keys) { [void]$presetBox.Items.Add($name) }
$presetBox.SelectedIndex = 0
$presetBox.Margin = New-Object Windows.Forms.Padding(3, 5, 3, 10)
$tip.SetToolTip($presetLabel, 'Sets every slider to a starting point for your kind of wheel. Fine-tune from there.')
$presetBox.Add_SelectedIndexChanged({
    if ($this.SelectedIndex -le 0) { return }
    $p = $presets[$this.SelectedItem]
    foreach ($k in $p.Keys) { if ($controls.ContainsKey($k)) { Show-Value $k $p[$k] } }
    $help.Text = 'Preset applied: ' + $this.SelectedItem + '. Fine-tune with the sliders.'
    Mark-Changed
})
# The point of this window: tune while driving. Said up front, where it cannot be missed.
$live = New-Object Windows.Forms.Label
$live.AutoSize = $false; $live.Width = 530; $live.Height = 46
$live.BackColor = [Drawing.Color]::FromArgb(220, 236, 255); $live.ForeColor = [Drawing.Color]::FromArgb(20, 60, 120)
$live.Font = New-Object Drawing.Font('Segoe UI', 9, [Drawing.FontStyle]::Bold)
$live.Padding = New-Object Windows.Forms.Padding(8, 4, 8, 4)
$live.Margin = New-Object Windows.Forms.Padding(0, 0, 0, 10)
$live.Text = 'Changes apply LIVE: keep DriveClub running, drive, and move a slider. The wheel feels the change within a second, no restart needed.'
$table.Controls.Add($live, 0, 0); $table.SetColumnSpan($live, 3)
$table.Controls.Add($presetLabel, 0, 1); $table.Controls.Add($presetBox, 1, 1)
# Only the chosen model's settings are shown.
function Update-Groups {
    if (-not $controls.ContainsKey('wheel_ffb_model')) { return }
    $physics = (Value-Of 'wheel_ffb_model') -eq 'physics'
    foreach ($s in $settings) {
        $c = $controls[$s.Key]
        if (-not $c) { continue }
        $show = (-not $s.Group) -or (($s.Group -eq 'physics') -eq $physics)
        $c.Label.Visible = $show; $c.Box.Visible = $show
        if ($c.Bar) { $c.Bar.Visible = $show }
    }
}
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
            $box.Add_SelectedIndexChanged({ if ($this.Tag -eq 'wheel_ffb_model') { Update-Groups }; if (-not $script:syncing) { Mark-Changed } })
            $box.Add_Enter({ $help.Text = $controls[$this.Tag].Setting.Help })
            $box.Margin = New-Object Windows.Forms.Padding(3, 5, 3, 5)
            $table.Controls.Add($box, 1, $row)
            $entry.Box = $box
        }
        'check' {
            $box = New-Object Windows.Forms.CheckBox
            $box.AutoSize = $true; $box.Tag = $s.Key
            $box.Add_CheckedChanged({ if (-not $script:syncing) { Mark-Changed } })
            $box.Add_Enter({ $help.Text = $controls[$this.Tag].Setting.Help })
            $box.Margin = New-Object Windows.Forms.Padding(3, 7, 3, 5)
            $table.Controls.Add($box, 1, $row)
            $entry.Box = $box
        }
    }
    $controls[$s.Key] = $entry
    $value = $loaded[$s.Key]
    if ($null -eq $value) { $value = $s.Default }
    Show-Value $s.Key $value
    $script:shown[$s.Key] = Value-Of $s.Key
    $row++
}
Update-Groups

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
$undo.Add_Click({ foreach ($s in $settings) { $v = $loaded[$s.Key]; if ($null -eq $v) { $v = $s.Default }; Show-Value $s.Key $v }; Mark-Changed })
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
