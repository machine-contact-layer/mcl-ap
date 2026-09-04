# Talk to the DFR1154 MCL node over its native USB CDC.
#
#   .\node.ps1 PING
#   .\node.ps1 SELFTEST
#   .\node.ps1 "SEND FRAME"
#   .\node.ps1 "LISTEN FRAME 1.5"
#
# Reads until the board goes quiet rather than until a fixed line count: the
# board's replies vary in length, and a reader that stops early truncates a
# decode result into something that looks like a failure.

param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Command,
    [int]$QuietMs = 2500,
    [int]$MaxMs = 60000,
    [string]$Port = 'COM3'
)

$ErrorActionPreference = 'Stop'

$sp = New-Object System.IO.Ports.SerialPort $Port, 921600, 'None', 8, 'One'
$sp.ReadTimeout = 250
$sp.NewLine = "`n"
# DTR/RTS must be asserted or the native CDC will not present data.
$sp.DtrEnable = $true
$sp.RtsEnable = $true
$sp.Open()

try {
    Start-Sleep -Milliseconds 300
    $sp.DiscardInBuffer()
    $sp.WriteLine($Command)

    $lastData = [DateTime]::UtcNow
    $started = [DateTime]::UtcNow
    $buffer = ''

    while ($true) {
        try {
            $chunk = $sp.ReadExisting()
        } catch {
            $chunk = ''
        }
        if ($chunk.Length -gt 0) {
            $buffer += $chunk
            $lastData = [DateTime]::UtcNow
        } else {
            Start-Sleep -Milliseconds 50
        }
        $quiet = ([DateTime]::UtcNow - $lastData).TotalMilliseconds
        $total = ([DateTime]::UtcNow - $started).TotalMilliseconds
        if ($quiet -ge $QuietMs -and $buffer.Length -gt 0) { break }
        if ($total -ge $MaxMs) { break }
    }

    $buffer -split "`r?`n" | Where-Object { $_.Trim().Length -gt 0 } | ForEach-Object {
        Write-Output $_.Trim()
    }
} finally {
    $sp.Close()
    $sp.Dispose()
}
