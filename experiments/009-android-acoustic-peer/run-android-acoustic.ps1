<#
    Experiment 009 - MCL-AP from an Android handset's speaker to a laptop
    microphone.

    THE RIG

      phone loudspeaker  --air-->  laptop microphone  -->  mcl_ap_node decode

    The phone plays a WAV that mcl_ap_node modulated; the laptop records the
    room and demodulates it with the same modem the DFR1154 runs. What crosses
    the room is an MCL Tier-0 object, or a Link frame carrying one.

    ONE DIRECTION ONLY, AND THE REASON IS ANDROID

    The phone emits and does not receive. Capture on Android requires
    RECORD_AUDIO, which belongs to an application: /dev/snd is system:audio and
    the adb shell user is not in the audio group, so no shell binary can open
    the microphone. Recording from the phone therefore needs an APK, and an APK
    is a larger claim than this experiment makes. The limit is Android's
    permission model, not the modem, and it is written here rather than left
    for a reader to infer from a missing table row.

    WHY THE PLAYBACK LOOKS SO CONVOLUTED

    Three findings, each of which produced captures that looked like a dead
    channel and were not:

      - A bare VIEW intent raises a "choose a player" dialog. It needs a tap,
        so the launch is explicit (-n) and no chooser can appear.
      - Re-launching the same activity gives "intent delivered to currently
        running top-most instance" and plays nothing. Each trial force-stops
        the player first.
      - With the screen off the activity never reaches the foreground and
        nothing plays. Each trial wakes the device.

    All three produce a silent recording. Reporting them as acoustic failures
    would have been the easiest mistake available here, which is why the run
    log records the peak level of every capture: a capture with no signal in it
    at all is an instrument fault, not a channel result.

    LEVEL

    -Volume is an index in 0..15 and defaults to 11. Not maximum, deliberately:
    the waveform is a continuous 2-6 kHz tone burst, which is not what a
    microspeaker is voiced for, and nothing in this experiment needs the last
    two steps of level. The script restores the volume it found.
#>
param(
    [int]$Trials = 10,
    [string]$Mic,
    [int]$Volume = 11,
    [int]$Window = 12,
    [string]$Adb = 'C:\Users\marsm\rdb\adb.exe',
    [string]$Node,
    [string]$EvidenceDir = ''
)

$ErrorActionPreference = 'Continue'

if ([string]::IsNullOrWhiteSpace($Mic))  { throw "-Mic is required. List devices with: ffmpeg -list_devices true -f dshow -i dummy" }
if ([string]::IsNullOrWhiteSpace($Node)) { throw "-Node is required: the path to mcl_ap_node.exe" }
if (-not (Test-Path -LiteralPath $Node)) { throw "mcl_ap_node not found at $Node" }
if ($Volume -lt 0 -or $Volume -gt 13) {
    throw "-Volume $Volume is outside the range this experiment will drive (0..13). The top of the scale is not needed and is not offered."
}
# 12.5 s is all mcl_ap_node can hold (MAX_SAMPLES). A longer window is silently
# unreadable, which looks like a decode failure and is not one.
if ($Window -gt 12) { throw "-Window must be 12 s or less: mcl_ap_node caps a capture at 600000 samples." }

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$stamp = Get-Date -Format 'yyyyMMdd'
if ([string]::IsNullOrWhiteSpace($EvidenceDir)) {
    $EvidenceDir = Join-Path $scriptDir "evidence\e3-android-speaker-$stamp"
}
New-Item -ItemType Directory -Force -Path $EvidenceDir | Out-Null
$EvidenceDir = (Resolve-Path $EvidenceDir).Path

$Player   = 'com.google.android.apps.nbu.files'
$Activity = "$Player/.gateway.preview.PreviewActivity"
$log      = Join-Path $EvidenceDir 'run.log'

function Log([string]$t) {
    Write-Host $t
    $t | Out-File -FilePath $log -Append -Encoding utf8
}

function PeakDb([string]$wav) {
    $line = ffmpeg -hide_banner -i $wav -af volumedetect -f null NUL 2>&1 | Select-String 'max_volume'
    if ($null -eq $line) { return 'unknown' }
    return ($line.ToString() -replace '.*max_volume:\s*', '').Trim()
}

# Generate the two waveforms from the canonical modem, then stage them on the
# phone. They are regenerated every run rather than kept as fixtures: a fixture
# is a copy of the protocol that can go stale.
$srcFrame = Join-Path $EvidenceDir 'source-frame.wav'
$srcWire  = Join-Path $EvidenceDir 'source-wire.wav'
& $Node gen-frame $srcFrame | Out-Null
& $Node gen-wire  $srcWire  | Out-Null
& $Adb push $srcFrame /sdcard/Music/mcl-ap-frame.wav 2>&1 | Out-Null
& $Adb push $srcWire  /sdcard/Music/mcl-ap-wire.wav  2>&1 | Out-Null

$startVolume = ((& $Adb shell "cmd media_session volume --stream 3 --get" 2>&1 |
                 Select-Object -Last 1) -replace '.*volume is (\d+).*', '$1')
& $Adb shell "svc power stayon usb" 2>&1 | Out-Null
for ($v = 0; $v -lt 20; $v++) { & $Adb shell "input keyevent KEYCODE_VOLUME_DOWN" 2>&1 | Out-Null }
for ($v = 0; $v -lt $Volume; $v++) { & $Adb shell "input keyevent KEYCODE_VOLUME_UP" 2>&1 | Out-Null }
$setVolume = ((& $Adb shell "cmd media_session volume --stream 3 --get" 2>&1 |
               Select-Object -Last 1) -replace '.*volume is (\d+).*', '$1')

'MCL-AP experiment 009 - Android speaker to laptop microphone' | Out-File -FilePath $log -Encoding utf8
Log ("started    {0}" -f (Get-Date -Format 'yyyy-MM-ddTHH:mm:sszzz'))
Log ("device     {0} {1}, Android {2} (API {3}), {4}" -f
     (& $Adb shell getprop ro.product.manufacturer).Trim(),
     (& $Adb shell getprop ro.product.model).Trim(),
     (& $Adb shell getprop ro.build.version.release).Trim(),
     (& $Adb shell getprop ro.build.version.sdk).Trim(),
     (& $Adb shell getprop ro.soc.model).Trim())
Log ("microphone {0}" -f $Mic)
Log ("volume     index {0} of 15 (was {1}); restored at the end" -f $setVolume, $startVolume)
Log ("waveform   FSK 3000/6000 Hz, chirp 2000-6000 Hz, 300 baud, 48 kHz")
Log ''

$summary = @()
foreach ($kind in @('wire', 'frame')) {
    $acq = 0; $rec = 0
    for ($i = 1; $i -le $Trials; $i++) {
        $wav = Join-Path $EvidenceDir ("{0}-trial-{1:d2}.wav" -f $kind, $i)

        & $Adb shell "input keyevent KEYCODE_WAKEUP" 2>&1 | Out-Null
        & $Adb shell "am force-stop $Player" 2>&1 | Out-Null
        Start-Sleep -Milliseconds 600

        $ff = Start-Process -FilePath 'ffmpeg' -PassThru -WindowStyle Hidden `
            -ArgumentList "-hide_banner -loglevel error -y -f dshow -i `"audio=$Mic`" -t $Window -ar 48000 -ac 1 `"$wav`""
        Start-Sleep -Milliseconds 1000
        & $Adb shell "am start -n $Activity -a android.intent.action.VIEW -d file:///sdcard/Music/mcl-ap-$kind.wav -t audio/wav" 2>&1 | Out-Null
        $ff.WaitForExit()
        Start-Sleep -Milliseconds 600

        $peak = PeakDb $wav
        $out  = ((& $Node "decode-$kind" $wav 2>&1) -join ' ')
        $rc   = $LASTEXITCODE
        if ($out -match 'acquired=1') { $acq++ }
        if ($rc -eq 0) { $rec++ }
        Log ("{0} {1:d2}  rc={2}  peak={3,-9} {4}" -f $kind, $i, $rc, $peak, $out)
    }
    $summary += [pscustomobject]@{ Kind = $kind; Acquired = "$acq/$Trials"; Recovered = "$rec/$Trials" }
}

# Put the volume back where it was found.
for ($v = 0; $v -lt 20; $v++) { & $Adb shell "input keyevent KEYCODE_VOLUME_DOWN" 2>&1 | Out-Null }
for ($v = 0; $v -lt [int]$startVolume; $v++) { & $Adb shell "input keyevent KEYCODE_VOLUME_UP" 2>&1 | Out-Null }
& $Adb shell "svc power stayon false" 2>&1 | Out-Null

Log ''
Log 'SUMMARY'
foreach ($r in $summary) {
    Log ("  {0,-6} acquired {1}  recovered {2}" -f $r.Kind, $r.Acquired, $r.Recovered)
}
Log ''
Log 'Acquired and recovered are counted separately on purpose. A cell that'
Log 'acquires and does not recover is a channel result; a cell that does not'
Log 'acquire at all is usually the harness, and the peak level above says which.'

$hashLines = @()
Get-ChildItem -File $EvidenceDir | Where-Object { $_.Name -ne 'SHA256SUMS.txt' } | Sort-Object Name | ForEach-Object {
    $hashLines += ("{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash, $_.Name)
}
[System.IO.File]::WriteAllLines((Join-Path $EvidenceDir 'SHA256SUMS.txt'), $hashLines,
                                (New-Object System.Text.UTF8Encoding($false)))
Write-Host ''
Write-Host "evidence: $EvidenceDir"
