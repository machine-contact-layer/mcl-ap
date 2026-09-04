# Experiment 004: over-air MCL between the DFR1154 and this laptop, both
# directions, with the protocol stack running on BOTH peers.
#
#   .\run-node-trials.ps1 -Direction board-to-host -Trials 10
#   .\run-node-trials.ps1 -Direction host-to-board -Trials 10
#
# board-to-host  the board builds a major-1 Link frame carrying a major-1
#                Tier-0 PRESENCE, modulates it and emits it; this laptop
#                records and decodes it.
#
# host-to-board  this laptop builds and emits the same shape of frame; the
#                BOARD demodulates, verifies and decodes it, and reports the
#                field values it read. Nothing is decoded here.
#
# The second direction is the one that matters. In board-to-host the board is
# a transmitter and the laptop still does the understanding, which firmware v2
# could already arrange with a recorded WAV. In host-to-board the
# microcontroller is the one that has to understand, and no host is in the
# loop at all.
#
# Captures and the log go into evidence/<name>/. Nothing is overwritten: a run
# into an existing directory is refused, because a re-run that quietly
# replaces a previous result is how a bad number becomes a good one.

param(
    [ValidateSet('board-to-host', 'host-to-board')]
    [string]$Direction = 'board-to-host',
    [int]$Trials = 10,
    [string]$Port = 'COM3',
    [string]$Mic = 'Microphone Array (Realtek(R) Audio)',
    [string]$Node = 'C:\Users\marsm\AppData\Local\Temp\mcl-ap-win\Release\mcl_ap_node.exe',
    [string]$Esptool = 'C:\Users\marsm\AppData\Roaming\Python\Python314\Scripts\esptool.exe',
    [string]$EvidenceName = '',
    [int]$PlayDelayMs = 50,
    # 'frame' sends a complete major-1 Link frame carrying a major-1 Tier-0
    # PRESENCE: 24 bytes. 'wire' sends the bare object: 10 bytes.
    #
    # Both are run because they separate two things that otherwise get
    # confused. The retained E3/E4 evidence carried an 11-byte payload; a
    # 24-byte frame is 2.2x the symbols across the same path, so a lower
    # recovery rate could mean a worse room OR simply more chances to lose a
    # bit. Running both on the same path in the same session is what tells
    # them apart.
    [ValidateSet('frame', 'wire')]
    [string]$Payload = 'frame',
    [switch]$SkipReset
)

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path

if ($EvidenceName -eq '') {
    $EvidenceName = "$Direction-$Payload-$(Get-Date -Format 'yyyyMMdd')"
}
$Evidence = Join-Path $Here "evidence\$EvidenceName"
if (Test-Path $Evidence) {
    throw "REFUSING: $Evidence already exists. A run that overwrites a previous result is how a bad number becomes a good one. Pass -EvidenceName for a new directory."
}
New-Item -ItemType Directory -Force -Path $Evidence | Out-Null

if (-not (Test-Path $Node)) { throw "no host node tool at $Node" }

# ----------------------------------------------------------------- reset
#
# The board is reset before every run, and this is not defensive tidiness.
# The ESP32-S3 native USB CDC does not survive the host closing and reopening
# the port: the first PING after a reset is answered, and every PING after a
# reopen is not. Measured, three DTR/RTS combinations, all silent. Resetting
# first makes each run start from the same state rather than from whatever
# the previous run left behind.
if (-not $SkipReset) {
    if (-not (Test-Path $Esptool)) { throw "esptool not found at $Esptool" }
    & $Esptool --chip esp32s3 --port $Port --after hard-reset chip-id | Out-Null
    Start-Sleep -Milliseconds 2500
}

# ---------------------------------------------------------------- serial
$sp = New-Object System.IO.Ports.SerialPort $Port, 921600, 'None', 8, 'One'
$sp.DtrEnable = $true
$sp.RtsEnable = $true
$sp.Open()
Start-Sleep -Milliseconds 400
$sp.DiscardInBuffer()

function Send-Line([string]$text) {
    $sp.Write("$text`n")
}

# Read until $marker appears or $timeoutMs elapses. Returns everything read.
function Read-Until([string]$marker, [int]$timeoutMs) {
    $buffer = ''
    $started = [DateTime]::UtcNow
    while ((([DateTime]::UtcNow - $started).TotalMilliseconds) -lt $timeoutMs) {
        $chunk = $sp.ReadExisting()
        if ($chunk.Length -gt 0) { $buffer += $chunk }
        if ($marker -ne '' -and $buffer -match [regex]::Escape($marker)) { break }
        Start-Sleep -Milliseconds 20
    }
    return $buffer
}

# As above but the marker is a regex, so a read can end on any of several
# terminal lines. The first version of the host-to-board loop stopped on the
# literal "MCLNODE LISTEN ", which the board's *progress* line matches -- so
# every trial was recorded as "not acquired" while the board was still
# decoding. It reported 0/2 for a reason that had nothing to do with sound.
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

$log = @()
$acquired = 0
$recovered = 0
$attempted = 0

function Log([string]$text) {
    Write-Host $text
    $script:log += $text
}

Log "=== MCL-AP Experiment 004: $Direction, payload=$Payload ==="
Log "date: $(Get-Date -Format 'yyyy-MM-ddTHH:mm:ssK')"
Log "port: $Port"
Log ''

Send-Line 'PING'
$pong = Read-Until 'MCLNODE PONG' 4000
if ($pong -notmatch 'MCLNODE PONG') {
    $sp.Close(); $sp.Dispose()
    throw "the board did not answer PING. Reset it and try again."
}
Log "board: $(($pong -split "`r?`n" | Where-Object { $_ -match 'MCLNODE' } | Select-Object -First 1).Trim())"
Log ''

# -------------------------------------------------- board -> host trials
if ($Direction -eq 'board-to-host') {
    for ($i = 1; $i -le $Trials; $i++) {
        $wav = Join-Path $Evidence ("trial-{0:d2}.wav" -f $i)
        $attempted++

        # The recorder starts first and is given time to actually open the
        # device. ffmpeg reports "start" long before the first sample.
        # One argument STRING, not an array. Start-Process joins an array
        # without quoting, so the device name -- which contains spaces and
        # parentheses -- arrived as several arguments and ffmpeg silently
        # produced no file. The first pilot run reported 0/2 recovered for
        # that reason and not for an acoustic one.
        $ffArgs = "-hide_banner -loglevel error -f dshow " +
                  "-i `"audio=$Mic`" -t 4 -ar 48000 -ac 1 " +
                  "-c:a pcm_s16le -y `"$wav`""
        $ff = Start-Process -FilePath 'ffmpeg' -PassThru -WindowStyle Hidden `
            -ArgumentList $ffArgs
        Start-Sleep -Milliseconds 1300

        $sp.DiscardInBuffer()
        Send-Line ("SEND " + $Payload.ToUpper())
        $out = Read-Until 'MCLNODE SEND DONE' 12000
        $ff.WaitForExit(15000) | Out-Null

        $sent = ($out -split "`r?`n" | Where-Object { $_ -match 'MCLNODE SEND ' -and $_ -match 'hex=' } | Select-Object -First 1)
        Log ("trial {0:d2} sent: {1}" -f $i, ($sent -replace 'MCLNODE ', '').Trim())

        $decode = & $Node ("decode-" + $Payload) $wav 2>&1
        $rc = $LASTEXITCODE
        foreach ($line in $decode) { Log ("           $line") }
        if ($rc -eq 0) { $recovered++; $acquired++ }
        elseif ($rc -eq 3) { $acquired++ }
        Log ''
    }
}

# -------------------------------------------------- host -> board trials
if ($Direction -eq 'host-to-board') {
    $source = Join-Path $Evidence 'host-source.wav'
    $gen = & $Node ("gen-" + $Payload) $source
    foreach ($line in $gen) { Log "source: $line" }
    Log ''

    $player = [System.Media.SoundPlayer]::new($source)
    $player.Load()

    for ($i = 1; $i -le $Trials; $i++) {
        $attempted++
        $sp.DiscardInBuffer()

        # 1.5 s is the board's whole buffer and its guard is 500 ms after
        # ARMED, so the capture window is [500, 2000] ms. The frame needs
        # 973 ms of that from the start of the preamble, which leaves about
        # half a second of placement freedom and no more.
        #
        # $PlayDelayMs is 50 and not 550 because SoundPlayer takes 490-570 ms
        # to actually start the device after Play() returns. A 550 ms delay
        # put the preamble 0.72 s into the capture, leaving too few samples
        # after it: the board acquired every trial at 0.56-0.60 correlation
        # and then failed with ERR_SYNC because the payload ran off the end
        # of the buffer. That is a scheduling defect, not an acoustic one,
        # and the separate acquired/recovered counters are what showed it.
        Send-Line ("LISTEN " + $Payload.ToUpper() + " 1.5")
        $armed = Read-Until 'MCLNODE LISTEN ARMED' 5000
        if ($armed -notmatch 'LISTEN ARMED') {
            Log ("trial {0:d2} FAILED: board did not arm" -f $i)
            continue
        }
        Start-Sleep -Milliseconds $PlayDelayMs
        $player.Play()

        # The board decodes on-chip: about 4.7 s for a 1.5 s capture, so the
        # wait is for a TERMINAL line, not for the first thing it says.
        $out = Read-UntilMatch 'MCLNODE LISTEN (RECOVERED|NO_RECOVERY|FAIL)' 40000
        Start-Sleep -Milliseconds 400
        $out += $sp.ReadExisting()
        $player.Stop()

        $lines = $out -split "`r?`n" | Where-Object { $_.Trim().Length -gt 0 }
        Log ("trial {0:d2}:" -f $i)
        foreach ($line in $lines) { Log ("           " + $line.Trim()) }

        if ($out -match 'acquired=1') { $acquired++ }
        if ($out -match 'MCLNODE LISTEN RECOVERED') { $recovered++ }
        Log ''
    }
}

$sp.Close()
$sp.Dispose()

Log '=== RESULT ==='
Log ("direction:  {0}" -f $Direction)
Log ("payload:    {0}" -f $Payload)
Log ("trials:     {0}" -f $attempted)
Log ("acquired:   {0}/{1}" -f $acquired, $attempted)
Log ("recovered:  {0}/{1}" -f $recovered, $attempted)
Log ''
Log 'Acquisition and recovery are reported separately on purpose. "Heard'
Log 'nothing" and "heard something and could not read it" are different'
Log 'problems, and only the second one means a link is nearly working.'

$log | Set-Content -Path (Join-Path $Evidence 'run.log') -Encoding utf8

Get-ChildItem $Evidence -Filter '*.wav' | ForEach-Object {
    "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash, $_.Name
} | Set-Content -Path (Join-Path $Evidence 'SHA256SUMS.txt') -Encoding utf8

Write-Host ''
Write-Host "evidence: $Evidence" -ForegroundColor Green
