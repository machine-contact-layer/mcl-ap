# Experiment 012: measure one transmitter's response, in ONE emission.
#
#   .\run-band-sweep.ps1 -Rig board
#   .\run-band-sweep.ps1 -Rig host-speaker
#
# WHY THE BURST BUDGET IS A PARAMETER AND NOT A CONVENTION
#
# These campaigns run in an occupied room. Experiment 011 ran forty-five
# emissions in the background without being asked to, and the objection was
# correct: a rig that can emit unboundedly will, because each individual
# emission looks cheap.
#
# So the budget is enforced in the script rather than remembered by whoever
# runs it. Every emission passes through Invoke-Burst, which decrements a
# counter and throws when it reaches zero. There is no path to the speaker
# that does not go through it.
#
# WHY THIS RIG DOES NOT CONVERGE THE CAPTURE GAIN
#
# Experiment 011's rig finds the right input gain by emitting probe tones and
# measuring the recorded peak. That is the right design when the budget is
# twenty trials and wrong when it is four: convergence would spend the whole
# budget deciding how loud to record.
#
# Instead the levels are FIXED and DECLARED. Both are written into the TSV
# header, and band_sweep refuses to compare two curves recorded at different
# settings -- because comparing dBFS across two input gains compares the gain
# settings, not the transmitters, and does it invisibly.
#
# WHAT ONE BURST BUYS
#
# The board emits a marker tone, a gap, and then twenty-seven tones of 100 ms
# each, streamed without a gap: 3.1 seconds of continuous sound, one burst,
# from which the whole response curve is recovered. The alternative -- a frame
# per candidate band -- is twenty-seven emissions for the same answer.

param(
    [ValidateSet('board', 'host-speaker')]
    [string]$Rig = 'board',

    # The hard ceiling on emissions for this invocation. Lower it freely;
    # raising it is a decision about someone else's room.
    [ValidateRange(1, 8)]
    [int]$MaxBursts = 4,

    [string]$Mic = 'Microphone Array (Realtek(R) Audio)',

    # Receiver input gain, FIXED for the campaign. Every path must be recorded
    # at this value or the curves cannot be placed alongside each other.
    # 0.12 is the cap experiment 011 arrived at: above it the board
    # amplifier's turn-on transient clips, and a clipped transient looks like
    # a strong signal rather than like silence.
    [double]$CaptureGain = 0.12,

    # Render volume for the host-speaker rig, as a percentage. It is a rig
    # control, not a protocol one. Held well below the top of the scale.
    [ValidateRange(1, 70)]
    [int]$Volume = 25,

    # Transmitter emission level, as a percentage of full scale. FIXED for the
    # campaign, for the same reason as CaptureGain.
    [ValidateRange(1, 100)]
    [int]$EmitGain = 50,

    [string]$Port = 'COM3',
    [string]$EvidenceName = '',
    [double]$RecordSeconds = 5.0,

    # The ladder WAV played by the host-speaker rig. Ignored by the board rig,
    # which generates the ladder in firmware.
    [string]$Ladder = (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'ladder.wav'),

    # The board is reset before the run, as experiment 011's rig does.
    # Repeated open/close of the native USB CDC can leave the port enumerated
    # and the firmware unresponsive -- which is indistinguishable, from the
    # host, from the wrong firmware being flashed. It happened here, and the
    # rig reported "not running firmware v5" about a board that was.
    [string]$Esptool = (Get-Command esptool -ErrorAction SilentlyContinue).Source,
    [switch]$SkipReset
)

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path

if ($EvidenceName -eq '') {
    $EvidenceName = "$Rig-$(Get-Date -Format 'yyyyMMdd')"
}
$Evidence = Join-Path $Here "evidence\$EvidenceName"
if (Test-Path $Evidence) {
    throw "REFUSING: $Evidence already exists. A run that overwrites a previous result is how a bad number becomes a good one. Pass -EvidenceName."
}
New-Item -ItemType Directory -Force -Path $Evidence | Out-Null
$Evidence = (Resolve-Path $Evidence).Path

$script:lines = @()
function Log([string]$t) { Write-Host $t; $script:lines += $t }

# --------------------------------------------------------------- burst budget
$script:BurstsLeft = $MaxBursts
$script:BurstsUsed = 0

function Invoke-Burst([string]$what, [scriptblock]$body) {
    if ($script:BurstsLeft -le 0) {
        throw "BURST BUDGET EXHAUSTED after $script:BurstsUsed emission(s). '$what' was not run. The room is occupied; raising -MaxBursts is a decision, not a retry."
    }
    $script:BurstsLeft--
    $script:BurstsUsed++
    Log ("burst {0}/{1}: {2}" -f $script:BurstsUsed, $MaxBursts, $what)
    & $body
}

# ---------------------------------------------------------------------- level
. (Join-Path $Here '..\011-bootstrap-over-air\audio-endpoint.ps1')

$startRender = $null
$startCapture = $null
function Get-Level([int]$flow) { [Ep]::Level($flow) }
function Set-Level([int]$flow, [double]$scalar) { [Ep]::Set($flow, [float]$scalar) }

# ffmpeg reports to stderr, and under -ErrorAction Stop Windows PowerShell
# turns each stderr line of a native command into a terminating error -- so a
# perfectly good capture aborts the run at the point where it is measured.
function PeakDb([string]$wav) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & ffmpeg -hide_banner -i $wav -af volumedetect -f null NUL 2>&1
        $line = $out | Select-String 'max_volume'
    } finally { $ErrorActionPreference = $prev }
    if ($null -eq $line) { return 'unknown' }
    return ($line.ToString() -replace '.*max_volume:\s*', '').Trim()
}

function Start-Capture([string]$wav) {
    $a = "-hide_banner -loglevel error -y -f dshow -audio_buffer_size 50 " +
         "-i audio=`"$Mic`" -t $RecordSeconds -ar 48000 -ac 1 -sample_fmt s16 `"$wav`""
    return Start-Process -FilePath 'ffmpeg' -PassThru -WindowStyle Hidden -ArgumentList $a
}

Log "=== experiment 012: multi-transmitter band sweep ==="
Log "rig:          $Rig"
Log "evidence:     $Evidence"
Log "burst budget: $MaxBursts"
Log "capture gain: $CaptureGain   (FIXED -- declared into the TSV)"
Log "emit gain:    $EmitGain%     (FIXED -- declared into the TSV)"
Log ""

$sp = $null
try {
    $startRender  = Get-Level 0
    $startCapture = Get-Level 1
    Log "levels on entry: render $startRender, capture $startCapture"
    Set-Level 1 $CaptureGain
    Log "capture gain set to $CaptureGain"

    $wav = Join-Path $Evidence 'sweep.wav'

    if ($Rig -eq 'board') {
        if (-not $SkipReset) {
            if (-not $Esptool -or -not (Test-Path $Esptool)) {
                throw 'esptool is not on PATH. Install it, or pass -Esptool with its full path.'
            }
            # Read-only: chip-id writes nothing. The reset is the point.
            & $Esptool --chip esp32s3 --port $Port --before default-reset --after hard-reset chip-id 2>&1 | Out-Null
            Log "board hard-reset before the run"
            Start-Sleep -Seconds 2
        }
        $sp = New-Object System.IO.Ports.SerialPort $Port, 921600, 'None', 8, 'One'
        $sp.ReadTimeout = 250
        $sp.NewLine = "`n"
        $sp.DtrEnable = $true
        $sp.RtsEnable = $true
        $sp.Open()
        # Asserting DTR/RTS on the native USB CDC/JTAG resets the board, and it
        # then takes about a second to boot. A 400 ms settle read back nothing
        # at all and the rig concluded the firmware was the wrong version --
        # a silent port and an old firmware look identical from here, so the
        # PING is retried before either is believed.
        Start-Sleep -Milliseconds 1500
        $sp.DiscardInBuffer()

        function Send-Line([string]$t) { $sp.Write("$t`n") }
        function Read-Until([string]$pat, [int]$ms) {
            $sw = [Diagnostics.Stopwatch]::StartNew()
            $buf = ''
            while ($sw.ElapsedMilliseconds -lt $ms) {
                try { $buf += $sp.ReadExisting() } catch {}
                if ($buf -match $pat) { return $buf }
                Start-Sleep -Milliseconds 20
            }
            return $buf
        }

        $pong = ''
        for ($try = 1; $try -le 3 -and $pong -notmatch 'MCLNODE PONG'; $try++) {
            $sp.DiscardInBuffer()
            Send-Line 'PING'
            $pong = Read-Until 'MCLNODE PONG' 3000
        }
        if ($pong -notmatch 'PONG v5') {
            throw "board is not running firmware v5 (SWEEP is a v5 command). Got: $($pong.Trim())"
        }
        Log "board: $($pong.Trim())"

        Send-Line "GAIN $EmitGain"
        Read-Until 'MCLNODE GAIN' 3000 | Out-Null

        Invoke-Burst "board tone ladder, 27 tones, 3.1 s continuous" {
            $sp.DiscardInBuffer()
            Send-Line 'SWEEP'
            $armed = Read-Until 'SWEEP ARMED' 5000
            if ($armed -notmatch 'SWEEP ARMED') { throw "board did not arm: $armed" }
            $hdr = ($armed -split "`n" | Where-Object { $_ -match 'MCLNODE SWEEP ' }) -join ' '
            Log "  $($hdr.Trim())"
            # The board waits 500 ms after ARMED before any sound, so the
            # recorder is running first.
            $ff = Start-Capture $wav
            $ff.WaitForExit()
            $done = Read-Until 'SWEEP DONE' 8000
            if ($done -notmatch 'SWEEP DONE') { Log "  WARNING: no SWEEP DONE" }
        }
    }
    else {
        # The host transmitter must emit THE SAME ladder the board does, or the
        # two curves are pictures of two different stimuli. band_sweep's
        # `ladder` subcommand generates it from the same constants the firmware
        # uses, and its structure is cross-checked against the firmware's in
        # the self-test.
        if (-not (Test-Path $Ladder)) {
            throw "no ladder at $Ladder -- generate it with: band_sweep ladder `"$Ladder`""
        }
        # Named $ladderCopy, not $ladder: PowerShell variable names are
        # case-insensitive, so assigning $ladder here silently overwrites
        # $Ladder -- the source path -- and Copy-Item is then asked to copy a
        # file onto itself before it exists. flash-app-only.ps1 carries the
        # same warning about $Port; this is the second time it has bitten.
        $ladderCopy = Join-Path $Evidence 'ladder.wav'
        Copy-Item -LiteralPath $Ladder -Destination $ladderCopy
        Log "ladder: $Ladder (copied into the evidence directory, so what was emitted is recorded next to what was heard)"
        Set-Level 0 ($Volume / 100.0)
        Log "render volume set to $Volume%"

        Invoke-Burst "host speaker tone ladder, 27 tones, 3.1 s continuous" {
            $ff = Start-Capture $wav
            Start-Sleep -Milliseconds 500
            & ffplay -hide_banner -loglevel error -autoexit -nodisp $ladderCopy 2>&1 | Out-Null
            $ff.WaitForExit()
        }
    }

    $peak = PeakDb $wav
    Log ""
    Log "capture: $wav"
    Log "peak:    $peak"
    if ($peak -ne 'unknown') {
        $p = [double]($peak -replace ' dB', '')
        if ($p -ge -0.5) {
            Log "  WARNING: CLIPPED. A clipped capture looks like a strong signal and is worse than silence. Lower -CaptureGain and re-run -- and re-run EVERY path, because the setting must match."
        } elseif ($p -lt -45.0) {
            Log "  WARNING: very quiet. The tones may not stand clear of the room."
        }
    }
    Log ""
    Log "bursts used: $script:BurstsUsed of $MaxBursts"
    Log ""
    Log "next, with no further emission:"
    Log "  band_sweep slots `"$wav`" $Rig --capgain $CaptureGain --emitgain $EmitGain > $Rig.tsv"
}
finally {
    if ($sp -and $sp.IsOpen) {
        try { $sp.Write("GAIN 1`n"); Start-Sleep -Milliseconds 200 } catch {}
        $sp.Close()
    }
    # Restore what was found, always. A rig that leaves the machine's volume
    # somewhere else is a rig that damages the next measurement -- and the
    # next person's ears.
    if ($null -ne $startRender)  { Set-Level 0 $startRender }
    if ($null -ne $startCapture) { Set-Level 1 $startCapture }
    Log "levels restored: render $startRender, capture $startCapture"
    $script:lines | Set-Content -Encoding utf8 (Join-Path $Evidence 'run.log')
}
