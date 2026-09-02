param(
    [string]$PortName = 'COM3',
    [string]$OutputPath = '',
    [string]$SourceWav = ''
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
public static class MclCaptureCrc32 {
    public static uint Compute(byte[] data) {
        uint crc = 0xFFFFFFFFu;
        foreach (byte value in data) {
            crc ^= value;
            for (int bit = 0; bit < 8; ++bit) {
                uint mask = 0u - (crc & 1u);
                crc = (crc >> 1) ^ (0xEDB88320u & mask);
            }
        }
        return ~crc;
    }
}
'@

$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$experimentDirectory = Resolve-Path (Join-Path $scriptDirectory '..\..')
if ([string]::IsNullOrWhiteSpace($SourceWav)) {
    $SourceWav = Join-Path $experimentDirectory 'exp001_e3_source.wav'
}
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $scriptDirectory 'dfr1154-e3-capture.wav'
}

if (-not (Test-Path -LiteralPath $SourceWav -PathType Leaf)) {
    throw "Source WAV not found: $SourceWav"
}
if (Test-Path -LiteralPath $OutputPath) {
    throw "Refusing to overwrite existing evidence: $OutputPath"
}

$device = Get-CimInstance Win32_PnPEntity |
    Where-Object { $_.Name -match "\($([regex]::Escape($PortName))\)$" -and $_.PNPDeviceID -like 'USB\VID_303A&PID_1001*' } |
    Select-Object -First 1
if ($null -eq $device) {
    throw "$PortName is not the expected ESP32-S3 native USB CDC/JTAG device (VID 303A, PID 1001)"
}

function Read-AsciiLine {
    param([System.IO.Ports.SerialPort]$Port)
    $bytes = [System.Collections.Generic.List[byte]]::new()
    while ($true) {
        $value = $Port.ReadByte()
        if ($value -eq 10) { break }
        if ($value -ne 13) {
            if ($bytes.Count -ge 512) { throw 'Serial control line exceeded 512 bytes' }
            $bytes.Add([byte]$value)
        }
    }
    return [Text.Encoding]::ASCII.GetString($bytes.ToArray())
}

function Read-ExactBytes {
    param(
        [System.IO.Ports.SerialPort]$Port,
        [int]$Count
    )
    $result = New-Object byte[] $Count
    $offset = 0
    while ($offset -lt $Count) {
        $request = [Math]::Min(4096, $Count - $offset)
        try {
            $read = $Port.Read($result, $offset, $request)
        } catch [TimeoutException] {
            throw "Serial payload timed out after $offset of $Count bytes"
        }
        if ($read -le 0) { throw "Serial stream ended after $offset of $Count bytes" }
        $offset += $read
    }
    return $result
}

$serial = [System.IO.Ports.SerialPort]::new($PortName, 921600, 'None', 8, 'One')
$serial.ReadBufferSize = 1048576
$serial.ReadTimeout = 15000
$serial.WriteTimeout = 5000
$serial.DtrEnable = $false
$serial.RtsEnable = $false
$player = [System.Media.SoundPlayer]::new($SourceWav)

try {
    $player.Load()
    $serial.Open()
    Start-Sleep -Milliseconds 300
    $serial.DiscardInBuffer()
    $serial.Write("CAPTURE`n")

    do {
        $line = Read-AsciiLine -Port $serial
        Write-Host "BOARD: $line"
        if ($line.StartsWith('MCLERROR ')) { throw $line }
    } until ($line -match '^MCLCAPTURE ARMED 48000 3$')

    # SoundPlayer has measurable device-start latency even after Load(). Start
    # it immediately; the board's fixed 500 ms guard makes microphone capture
    # begin before the source reaches the speaker while keeping the preamble in
    # the decoder's frozen first-0.5-second acquisition window.
    $player.Play()

    do {
        $line = Read-AsciiLine -Port $serial
        Write-Host "BOARD: $line"
        if ($line.StartsWith('MCLERROR ')) { throw $line }
    } until ($line -match '^MCLWAV ([0-9]+) ([0-9A-F]{8})$')

    $wavSize = [int]$Matches[1]
    $expectedCrc32 = [Convert]::ToUInt32($Matches[2], 16)
    if ($wavSize -lt 44 -or $wavSize -gt 400000) {
        throw "Board reported invalid WAV size $wavSize"
    }
    $wav = Read-ExactBytes -Port $serial -Count $wavSize
    $actualCrc32 = [MclCaptureCrc32]::Compute($wav)
    if ($actualCrc32 -ne $expectedCrc32) {
        throw ('WAV transport CRC-32 mismatch: expected {0:X8}, received {1:X8}' -f $expectedCrc32, $actualCrc32)
    }

    $endSeen = $false
    for ($i = 0; $i -lt 5; ++$i) {
        $line = Read-AsciiLine -Port $serial
        if ($line -eq 'MCLEND') {
            $endSeen = $true
            break
        }
        if ($line.Length -ne 0) {
            throw "Expected MCLEND; received '$line'"
        }
    }
    if (-not $endSeen) { throw 'MCLEND was not received after the WAV payload' }

    [IO.File]::WriteAllBytes($OutputPath, $wav)

    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $OutputPath).Hash
    Write-Host "CAPTURE_SAVED=$OutputPath" -ForegroundColor Green
    Write-Host "CAPTURE_SHA256=$hash"
    Write-Host ('CAPTURE_CRC32={0:X8}' -f $actualCrc32)
} finally {
    $player.Stop()
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
    & 'C:\Users\marsm\Desktop\RESTORE_MCL_AUDIO.ps1'
}
