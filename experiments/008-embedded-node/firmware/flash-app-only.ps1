# Flash the MCL embedded node firmware to the DFR1154 APPLICATION PARTITION ONLY.
#
# Every guard here exists because the failure it prevents is expensive: this
# board's factory application, bootloader and partition table are not
# recoverable from the internet.
#
#   - refuses without the verified factory backup, so there is always a way back
#   - refuses if COM3 is not the expected ESP32-S3 native USB CDC/JTAG device
#   - writes 0x20000 only, never the bootloader and never the partition table
#   - verifies the write, because an unverified flash is a guess
#
# It uses esptool directly. The Arduino CLI upload step can also rewrite the
# bootloader and the partition table, which is exactly what must not happen.
#
# Afterwards the board is restored with
# MCL_DFR1154_BACKUP_20260902\RESTORE_DFR1154_APP.cmd.

$ErrorActionPreference = 'Stop'

$Here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$Image   = Join-Path $Here 'build\dfr1154_mcl_node.ino.bin'
$Offset  = '0x20000'
$Port    = 'COM3'
$Esptool = (Get-Command esptool -ErrorAction SilentlyContinue).Source

$BackupDir   = (Join-Path $env:USERPROFILE 'Downloads\MCL_DFR1154_BACKUP_20260902')
$BackupImage = Join-Path $BackupDir 'dfr1154-factory-app-before-mcl.bin'
$BackupSha   = 'BC9D54037F9BC4ABADFC560EA40CC14406DD9FEE5D0376C273C9DFA1E10458F8'

if (-not $Esptool -or -not (Test-Path -LiteralPath $Esptool -PathType Leaf)) {
    throw 'esptool is not on PATH. Install it, or pass -Esptool with its full path.'
}
if (-not (Test-Path -LiteralPath $Image -PathType Leaf)) {
    throw "no firmware image at $Image -- run build.ps1 first"
}

# The way back must exist, and must be the image it claims to be.
if (-not (Test-Path -LiteralPath $BackupImage -PathType Leaf)) {
    throw "REFUSING: no factory backup at $BackupImage"
}
$backupHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $BackupImage).Hash
if ($backupHash -ne $BackupSha) {
    throw "REFUSING: factory backup hash mismatch: $backupHash"
}

# COM3 must be the board, not whatever else claimed the name today.
# Named $portDevice, not $port: PowerShell variable names are case-insensitive,
# so assigning $port here silently overwrote $Port -- the port NAME -- and
# esptool received a whole device description where 'COM3' belonged. It
# reported "Missing command", which is a long way from the actual cause.
$portDevice = Get-CimInstance Win32_PnPEntity |
    Where-Object { $_.Name -match '\(COM3\)$' -and $_.PNPDeviceID -like 'USB\VID_303A&PID_1001*' } |
    Select-Object -First 1
if ($null -eq $portDevice) {
    throw 'REFUSING: COM3 is not the expected ESP32-S3 native USB CDC/JTAG device (VID 303A, PID 1001)'
}

$imageHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Image).Hash
$imageSize = (Get-Item -LiteralPath $Image).Length

Write-Host 'Flashing the MCL embedded node firmware, application partition only.' -ForegroundColor Yellow
Write-Host "  image   $Image"
Write-Host "  size    $imageSize bytes"
Write-Host "  sha256  $imageHash"
Write-Host "  offset  $Offset"
Write-Host "  port    $Port  ($($portDevice.Name))"
Write-Host '  factory backup verified; bootloader, partition table and NVS are not written.'

& $Esptool --chip esp32s3 --port $Port --baud 921600 write-flash $Offset $Image
if ($LASTEXITCODE -ne 0) { throw "esptool write-flash failed with exit code $LASTEXITCODE" }

& $Esptool --chip esp32s3 --port $Port --baud 921600 verify-flash $Offset $Image
if ($LASTEXITCODE -ne 0) { throw "esptool verify-flash failed with exit code $LASTEXITCODE" }

Write-Host ''
Write-Host 'MCL NODE FIRMWARE FLASHED AND VERIFIED' -ForegroundColor Green
Write-Host "Restore the factory application with $BackupDir\RESTORE_DFR1154_APP.cmd"
