<#
    Experiment 011 - the three major-1 bootstrap objects over air.

    TWO RIGS, AND THE DIFFERENCE BETWEEN THEM IS THE POINT

      host-loopback   this laptop's speaker -> this laptop's microphone
      board           the DFR1154's speaker -> this laptop's microphone

    Both are over air. They differ in one term and only one: in host-loopback
    the transmitter and the receiver are driven by the SAME crystal, so the
    received symbol rate is exactly the nominal 160.0 samples per bit; in the
    board rig two independent oscillators set it, and the received rate is
    160.0 times a clock ratio nobody controls.

    Experiment 010c found that term by accident. After blind refinement the
    estimated rate settled at 0.045-0.058 away from nominal rather than at
    zero, and one trial's deviation GREW while its CRC went from failing to
    passing -- which only makes sense if part of the deviation is real. That
    made "estimator error against 160.0" two quantities added together, of
    which only one can be estimated away.

    Running both rigs is how they are separated. host-loopback measures the
    length effect with the clock term set to zero. The board rig adds it back.
    Neither number alone says what a bootstrap profile has to tolerate.

    EVERY CAPTURE IS DECODED TWICE

    bootstrap_air reports the shipped receiver and the blind refinement side by
    side on the same signal, so the remedy is measured rather than inferred
    from a previous session in a different room.

    LEVEL

    -Volume is a Windows master-volume percentage. It defaults to 50: the
    waveform is a continuous 3-6 kHz tone burst, which is not what a laptop
    speaker is voiced for, and nothing here needs the top of the scale. The
    script restores the level it found.
#>
param(
    [ValidateSet('host-loopback', 'board')]
    [string]$Rig = 'host-loopback',
    [ValidateSet('presence', 'accept', 'offer', 'all')]
    [string]$Cell = 'all',
    [int]$Trials = 20,
    [string]$Mic = 'Microphone Array (Realtek(R) Audio)',
    [int]$Volume = 25,
    # Emitted level, as a percentage of the encoder's full scale. It scales the
    # samples on their way to the amplifier and does NOT change the waveform:
    # two implementations at different volumes are both conforming, so this is
    # a rig control and not a protocol one.
    #
    # It defaults low because these campaigns run in rooms with people in them.
    # The receiver's input gain is converged automatically before any trial is
    # counted, so a quieter emission is met by a higher capture gain and the
    # recorded peak lands in the same window; what actually changes is the
    # signal-to-noise ratio, which is a property of the cell and is therefore
    # written into the run log rather than left implicit.
    [ValidateRange(1, 100)]
    [int]$Gain = 50,
    # Ceiling on the receiver's input gain.
    #
    # THE TRANSMITTER HAS A TRANSIENT AND IT DOES NOT SCALE WITH EMISSION GAIN.
    # The board's amplifier produces a brief click when its stream starts. That
    # click is amplified by the CAPTURE gain like everything else, but it is not
    # attenuated by the EMISSION gain, because it does not come from the samples.
    #
    # So driving the emission quieter and letting the input gain rise to
    # compensate eventually clips on the click rather than on the signal: at
    # capture gain 0.271 three consecutive trials peaked at -0.3 dB with the
    # room measured at -38.8 dB mean, and correlation fell from 0.86 to 0.72.
    # Capping the input gain bounds that, at the cost of a lower recorded peak.
    [double]$MaxCaptureGain = 0.15,
    # Set the receiver input gain DIRECTLY and skip convergence.
    #
    # Convergence is the right design when the budget is twenty trials: it
    # spends a few probe emissions to land the recorded peak in a good window.
    # It is the wrong design when the budget is four, because each probe IS an
    # emission and the room is occupied -- the rig would spend the whole budget
    # deciding how loud to record and never run the cell.
    #
    # Supplying this pins the gain to a value measured earlier and emits
    # nothing to find it. 0 means converge as before.
    [double]$FixedCaptureGain = 0.0,
    [string]$Port = 'COM3',
    # The FSK pair, in Hz. Empty means the modem default (3000/6000). The
    # default pair was measured on the board's speaker; experiment 002 measured
    # a notch around 3 kHz in this laptop's speaker, so the band is a cell of
    # this campaign and not a constant in it.
    [string]$Band = '',
    [string]$Tool = (Join-Path $env:TEMP 'mcl-ap-win\Release\mcl_ap_exp011_bootstrap_air.exe'),
    [string]$Esptool = (Get-Command esptool -ErrorAction SilentlyContinue).Source,
    [string]$EvidenceName = '',
    [switch]$SkipReset
)

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not (Test-Path -LiteralPath $Tool)) { throw "no bootstrap_air tool at $Tool" }
if ($Volume -lt 0 -or $Volume -gt 70) {
    throw "-Volume $Volume is outside the range this experiment will drive (0..70). The top of the scale is not needed and is not offered."
}

if ($EvidenceName -eq '') {
    $EvidenceName = "$Rig-$Cell-$(Get-Date -Format 'yyyyMMdd')"
}
$Evidence = Join-Path $Here "evidence\$EvidenceName"
if (Test-Path $Evidence) {
    throw "REFUSING: $Evidence already exists. A run that overwrites a previous result is how a bad number becomes a good one. Pass -EvidenceName for a new directory."
}
New-Item -ItemType Directory -Force -Path $Evidence | Out-Null
$Evidence = (Resolve-Path $Evidence).Path
$log = Join-Path $Evidence 'run.log'

$script:lines = @()
function Log([string]$t) {
    Write-Host $t
    $script:lines += $t
}

# A capture with no signal in it at all is an instrument fault, not a channel
# result. Experiment 009 learned this the expensive way, so every capture's
# peak level is recorded next to its decode.
function PeakDb([string]$wav) {
    # ffmpeg writes its whole report to stderr, and under -ErrorAction Stop
    # Windows PowerShell turns each stderr line of a native command into a
    # terminating NativeCommandError -- so a perfectly good capture aborts the
    # run at the point where it is being measured. The preference is relaxed
    # for this call only.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & ffmpeg -hide_banner -i $wav -af volumedetect -f null NUL 2>&1
        $line = $out | Select-String 'max_volume'
    } finally {
        $ErrorActionPreference = $prev
    }
    if ($null -eq $line) { return 'unknown' }
    return ($line.ToString() -replace '.*max_volume:\s*', '').Trim()
}

# ------------------------------------------------------------------- level
. (Join-Path $Here 'audio-endpoint.ps1')

$startRender = $null
$startCapture = $null

function Set-Level([int]$flow, [double]$scalar) { [Ep]::Set($flow, [float]$scalar) }

# Peak of a short probe capture, in dB. Returns $null if ffmpeg said nothing.
function Probe-Peak([string]$wav, [double]$seconds, [scriptblock]$emit) {
    $ffArgs = "-hide_banner -loglevel error -f dshow " +
              "-i `"audio=$Mic`" -t $seconds -ar 48000 -ac 1 " +
              "-c:a pcm_s16le -y `"$wav`""
    $ff = Start-Process -FilePath 'ffmpeg' -PassThru -WindowStyle Hidden -ArgumentList $ffArgs
    Start-Sleep -Milliseconds 1300
    & $emit
    $ff.WaitForExit(20000) | Out-Null
    $p = PeakDb $wav
    if ($p -eq 'unknown') { return $null }
    return [double]($p -replace ' dB', '')
}

<#
    AUTOMATIC INPUT LEVEL, AND WHY IT IS NOT OPTIONAL

    The first board run of this experiment recorded every capture at a peak of
    exactly 0.0 dB -- clipped. Clipping flattens the tone tops and manufactures
    harmonics the demodulator then decides against, so it degrades recovery
    while looking like a strong signal.

    The corpus this campaign is compared against was captured at -0.7 dB (008)
    and -3.5 dB (003) peak, so the rig converges the input gain into a window
    around that before any trial is counted, and records what it settled on.
    Without this step a change in recovery between sessions could be the
    payload length, the band, or simply the microphone slider.
#>
function Set-InputLevelFor([scriptblock]$emit) {
    # Wider and lower than the corpus window, because the input gain is capped:
    # see -MaxCaptureGain. A peak nearer -12 dB with an unclipped transient is a
    # better capture than one at -1 dB whose loudest sample is the amplifier.
    $targetLow = -14.0
    $targetHigh = -4.0
    $probe = Join-Path $Evidence 'level-probe.wav'
    $level = [Ep]::Level([Ep]::Capture)
    $settled = $false

    for ($attempt = 1; $attempt -le 7; $attempt++) {
        $peak = Probe-Peak $probe 3 $emit
        if ($null -eq $peak) { Log "  level probe $attempt : no capture"; break }
        Log ("  level probe {0}: capture gain {1:n3} -> peak {2,6:n1} dB" -f $attempt, $level, $peak)
        if ($peak -ge $targetLow -and $peak -le $targetHigh) { $settled = $true; break }

        # The Windows scalar is not linear in dB, so this steps rather than
        # solves: halve the gain when clipped, and scale toward the target
        # otherwise. Seven attempts is enough for the range these rigs use.
        if ($peak -gt $targetHigh) {
            $level = $level * 0.55
        } else {
            $level = $level * [math]::Pow(10.0, (($targetLow + $targetHigh) / 2.0 - $peak) / 40.0)
        }
        if ($level -gt $MaxCaptureGain) {
            $level = $MaxCaptureGain
            Set-Level ([Ep]::Capture) $level
            Log ("  input gain capped at {0:n3}; raising it further would clip on the transmitter transient" -f $level)
            break
        }
        if ($level -lt 0.005) { Log '  input gain hit its floor'; break }
        Set-Level ([Ep]::Capture) $level
    }
    Remove-Item -LiteralPath $probe -ErrorAction SilentlyContinue
    if (-not $settled) {
        Log "  WARNING: input level did not settle into [$targetLow, $targetHigh] dB."
        Log '  Peaks below are recorded as measured; a 0.0 dB peak is a clipped'
        Log '  capture and its decode is an instrument result, not a channel one.'
    }
    return [Ep]::Level([Ep]::Capture)
}

try {
    $startCapture = [Ep]::Level([Ep]::Capture)
    if ($startCapture -lt 0.0 -or $startCapture -gt 1.0) {
        throw "capture level read back as $startCapture, which is not a 0..1 scalar"
    }
} catch {
    $startCapture = $null
    Log "capture level control unavailable: $($_.Exception.Message)"
}

if ($Rig -eq 'host-loopback') {
    try {
        $startRender = [Ep]::Level([Ep]::Render)
        if ($startRender -lt 0.0 -or $startRender -gt 1.0) {
            throw "render level read back as $startRender, which is not a 0..1 scalar"
        }
        Set-Level ([Ep]::Render) ($Volume / 100.0)
    } catch {
        $startRender = $null
        Log "render level control unavailable, leaving it alone: $($_.Exception.Message)"
    }
}

# -------------------------------------------------------------------- board
$sp = $null
if ($Rig -eq 'board') {
    # The board is reset before the run for the reason experiment 008 records:
    # the ESP32-S3 native USB CDC does not survive the host closing and
    # reopening the port, so each run starts from the same state rather than
    # from whatever the previous run left behind.
    if (-not $SkipReset) {
        if (-not $Esptool -or -not (Test-Path $Esptool)) {
            throw 'esptool is not on PATH. Install it, or pass -Esptool with its full path.'
        }
        & $Esptool --chip esp32s3 --port $Port --after hard-reset chip-id | Out-Null
        Start-Sleep -Milliseconds 2500
    }
    $sp = New-Object System.IO.Ports.SerialPort $Port, 921600, 'None', 8, 'One'
    $sp.DtrEnable = $true
    $sp.RtsEnable = $true
    $sp.Open()
    Start-Sleep -Milliseconds 400
    $sp.DiscardInBuffer()
}

function Send-Line([string]$text) { $sp.Write("$text`n") }

function Read-UntilMatch([string]$pattern, [int]$timeoutMs) {
    $buffer = ''
    $started = [DateTime]::UtcNow
    while ((([DateTime]::UtcNow - $started).TotalMilliseconds) -lt $timeoutMs) {
        $chunk = $sp.ReadExisting()
        if ($chunk.Length -gt 0) { $buffer += $chunk }
        if ($buffer -match $pattern) { break }
        Start-Sleep -Milliseconds 20
    }
    return $buffer
}

Log "=== MCL-AP Experiment 011: bootstrap objects over air ==="
Log "date:       $(Get-Date -Format 'yyyy-MM-ddTHH:mm:ssK')"
Log "rig:        $Rig"
Log "microphone: $Mic"
if ($Rig -eq 'host-loopback') {
    if ($null -eq $startRender) {
        Log "output:     NOT SET by this script -- whatever the machine was at"
    } else {
        Log "output:     master volume $Volume% (was $([math]::Round(100 * $startRender))%); restored at the end"
    Log "            deliberately low -- these runs happen in occupied rooms"
    }
    Log "clock:      transmitter and receiver share this host's crystal -- the"
    Log "            independent-oscillator term is ABSENT from these numbers"
} else {
    Log "port:       $Port"
    Log "emit gain:  $Gain% of full scale (rig control; the waveform is unchanged)"
    Log "input cap:  capture gain <= $MaxCaptureGain, to keep the amplifier transient unclipped"
    Log "clock:      board and host oscillators are independent -- this rig"
    Log "            carries the clock term host-loopback cannot show"
}
if ($Band -eq '') {
    Log "waveform:   FSK 3000/6000 Hz (modem default), 300 baud, 48 kHz"
} else {
    Log "waveform:   FSK $Band Hz (band override), 300 baud, 48 kHz"
}
Log ''

if ($Rig -eq 'board') {
    Send-Line 'PING'
    $pong = Read-UntilMatch 'MCLNODE PONG' 4000
    if ($pong -notmatch 'MCLNODE PONG') {
        $sp.Close(); $sp.Dispose()
        throw 'the board did not answer PING. Reset it and try again.'
    }
    Log "board:      $((($pong -split "`r?`n" | Where-Object { $_ -match 'MCLNODE' } | Select-Object -First 1)).Trim())"
    $sp.DiscardInBuffer()
    Send-Line "GAIN $Gain"
    $gainReply = Read-UntilMatch 'MCLNODE GAIN|MCLNODE ERROR' 3000
    if ($gainReply -notmatch 'MCLNODE GAIN') {
        $sp.Close(); $sp.Dispose()
        throw "the board refused GAIN $Gain. A run whose emitted level is unknown is not a measurement."
    }
    Log "gain:       $((($gainReply -split "`r?`n" | Where-Object { $_ -match 'MCLNODE GAIN' } | Select-Object -First 1)).Trim())"

    # THE BOARD HAS TO BE TOLD THE BAND. IT WAS NOT.
    #
    # -Band reconfigured the HOST tool -- gen, hex and decode -- and nothing
    # else, so the board went on emitting at the modem default while the host
    # decoded against the override. Two runs at two different bands both came
    # back acquired=0 with correlation ~0.10 and healthy peaks, which is
    # exactly what a transmit/receive band mismatch looks like, and both were
    # briefly mistaken for a property of the bands.
    #
    # It is a hard failure, not a warning. A rig that cannot set the band it
    # claims to be testing produces numbers about a different band.
    if ($Band -ne '') {
        $sp.DiscardInBuffer()
        $bandParts = $Band -split ':'
        if ($bandParts.Count -ne 2) {
            $sp.Close(); $sp.Dispose()
            throw "-Band must be <f0>:<f1> in Hz, got '$Band'"
        }
        Send-Line "BAND $($bandParts[0]) $($bandParts[1])"
        $bandReply = Read-UntilMatch 'MCLNODE BAND|MCLNODE ERROR' 3000
        if ($bandReply -notmatch 'MCLNODE BAND') {
            $sp.Close(); $sp.Dispose()
            throw "the board refused BAND $Band. A run whose emitted band is unknown is not a measurement."
        }
        Log "band:       $((($bandReply -split "`r?`n" | Where-Object { $_ -match 'MCLNODE BAND' } | Select-Object -First 1)).Trim())"
    }
    Log ''
}

$cells = if ($Cell -eq 'all') { @('presence', 'accept', 'offer') } else { @($Cell) }
$summary = @()

foreach ($c in $cells) {
    $bandArgs = @()
    if ($Band -ne '') { $bandArgs = @('--band', $Band) }
    $expected = (& $Tool @bandArgs hex $c).Trim()
    $bytes = $expected.Length / 2
    Log "--- cell $c ($bytes bytes) expected=$expected ---"

    $source = Join-Path $Evidence "source-$c.wav"
    $gen = & $Tool @bandArgs gen $c $source
    foreach ($l in $gen) { Log "source: $l" }

    $player = $null
    if ($Rig -eq 'host-loopback') {
        $player = [System.Media.SoundPlayer]::new($source)
        $player.Load()
    }

    # What one trial emits. Named once so the level probe and the trials
    # transmit the same thing -- a level converged on a different signal is
    # not a level for this one.
    $emit = if ($Rig -eq 'host-loopback') {
        { $player.PlaySync() }
    } else {
        {
            $sp.DiscardInBuffer()
            Send-Line "SEND HEX $expected"
            Read-UntilMatch 'MCLNODE SEND DONE|MCLNODE ERROR' 12000 | Out-Null
        }
    }

    if ($null -ne $startCapture) {
        if ($FixedCaptureGain -gt 0.0) {
            Set-Level ([Ep]::Capture) $FixedCaptureGain
            $settledAt = [Ep]::Level([Ep]::Capture)
            Log ("  input gain FIXED at {0:n3}; no probe emissions" -f $settledAt)
        } else {
            $settledAt = Set-InputLevelFor $emit
        }
        Log ("  capture gain settled at {0:n3} (was {1:n3})" -f $settledAt, $startCapture)
    } else {
        Log '  capture gain NOT SET by this script'
    }

    $acq = 0; $stock = 0; $refined = 0
    for ($i = 1; $i -le $Trials; $i++) {
        $wav = Join-Path $Evidence ("$c-trial-{0:d2}.wav" -f $i)

        # One argument STRING, not an array: Start-Process joins an array
        # without quoting, so a device name containing spaces and parentheses
        # arrives as several arguments and ffmpeg silently produces no file.
        $ffArgs = "-hide_banner -loglevel error -f dshow " +
                  "-i `"audio=$Mic`" -t 4 -ar 48000 -ac 1 " +
                  "-c:a pcm_s16le -y `"$wav`""
        $ff = Start-Process -FilePath 'ffmpeg' -PassThru -WindowStyle Hidden `
              -ArgumentList $ffArgs
        Start-Sleep -Milliseconds 1300

        & $emit

        $ff.WaitForExit(15000) | Out-Null
        Start-Sleep -Milliseconds 200

        $peak = PeakDb $wav
        $out = ((& $Tool @bandArgs decode $c $wav 2>&1) -join ' ')
        $rc = $LASTEXITCODE
        if ($rc -ne 4) { $acq++ }
        if ($rc -eq 0) { $stock++; $refined++ }
        elseif ($rc -eq 1) { $refined++ }
        $clipped = if ($peak -match '^-?0\.0 dB$') { ' CLIPPED' } else { '' }
        Log ("  {0} {1:d2} rc={2} peak={3,-9}{4} {5}" -f $c, $i, $rc, $peak, $clipped, ($out -replace [regex]::Escape($Evidence), '.'))
    }

    Log ''
    Log ("  $c : acquired $acq/$Trials   shipped receiver $stock/$Trials   blind refinement $refined/$Trials")
    Log ''
    $summary += [pscustomobject]@{
        Cell = $c; Bytes = $bytes; Acquired = "$acq/$Trials"
        Shipped = "$stock/$Trials"; Refined = "$refined/$Trials"
    }
}

# Both endpoints go back where they were found.
if ($null -ne $startRender) { try { Set-Level ([Ep]::Render) $startRender } catch { } }
if ($null -ne $startCapture) { try { Set-Level ([Ep]::Capture) $startCapture } catch { } }
if ($null -ne $sp) { $sp.Close(); $sp.Dispose() }

Log 'SUMMARY'
Log ('  {0,-9} {1,5}  {2,10}  {3,10}  {4,10}' -f 'cell', 'bytes', 'acquired', 'shipped', 'refined')
foreach ($r in $summary) {
    Log ('  {0,-9} {1,5}  {2,10}  {3,10}  {4,10}' -f $r.Cell, $r.Bytes, $r.Acquired, $r.Shipped, $r.Refined)
}
Log ''
Log 'Acquired, shipped and refined are counted separately on purpose. A cell'
Log 'that acquires and does not recover is a channel result; a cell that does'
Log 'not acquire at all is usually the harness, and the peak level says which.'
Log 'Shipped and refined come from the SAME capture, so their difference is the'
Log 'estimator and nothing else.'

[System.IO.File]::WriteAllLines($log, $script:lines, (New-Object System.Text.UTF8Encoding($false)))

$hashLines = @()
Get-ChildItem -File $Evidence | Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    Sort-Object Name | ForEach-Object {
        $hashLines += ('{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash, $_.Name)
    }
[System.IO.File]::WriteAllLines((Join-Path $Evidence 'SHA256SUMS.txt'), $hashLines,
                                (New-Object System.Text.UTF8Encoding($false)))
Write-Host ''
Write-Host "evidence: $Evidence"
