# Build the MCL embedded node firmware. Builds only -- never uploads.
#
# WHY THE SOURCES ARE STAGED RATHER THAN COPIED INTO THE SKETCH
#
# The board must run THE SAME source the host runs. If the MCL sources lived
# in the sketch directory as their own copies they would drift, and the claim
# "one implementation compiled twice" would quietly stop being true.
#
# So this script copies them from their canonical repositories at build time,
# and prints the SHA-256 of every staged file next to the SHA-256 of the
# resulting image. The evidence record names those hashes, so what ran on the
# board is identifiable rather than asserted.
#
# WHY IT DOES NOT UPLOAD
#
# The Arduino CLI upload step can rewrite the bootloader and the partition
# table. This board is flashed application-partition-only, at 0x20000, with
# esptool, and the command is printed at the end rather than run -- flashing
# is a decision, not a build step.

$ErrorActionPreference = 'Stop'

$Here      = Split-Path -Parent $MyInvocation.MyCommand.Path
$Sketch    = Join-Path $Here 'dfr1154_mcl_node'
$Staged    = Join-Path $Sketch 'src'
$Root      = (Resolve-Path (Join-Path $Here '..\..\..\..')).Path
$BuildPath = Join-Path $Here 'build'

# The board profile. CDCOnBoot=cdc is not optional: without it `Serial` is
# UART0 rather than the native USB CDC/JTAG the board enumerates as (VID 303A,
# PID 1001), and every line this firmware prints would go to pins nobody is
# reading. Firmware v2 was built the same way.
$Fqbn = 'esp32:esp32:esp32s3:CDCOnBoot=cdc'

$Backup = (Join-Path $env:USERPROFILE 'Downloads\MCL_DFR1154_BACKUP_20260902\dfr1154-factory-app-before-mcl.bin')

Write-Host '=== MCL embedded node firmware ===' -ForegroundColor Cyan
Write-Host "root:   $Root"
Write-Host "sketch: $Sketch"
Write-Host ''

if (-not (Test-Path $Backup)) {
    Write-Host "REFUSING TO BUILD: no factory backup at" -ForegroundColor Red
    Write-Host "  $Backup"
    Write-Host ''
    Write-Host 'The build itself is harmless, but the only reason to build is to'
    Write-Host 'flash, and this board must not be flashed without a restorable'
    Write-Host 'factory application. Take the backup first.'
    exit 1
}
Write-Host "factory backup present: $Backup" -ForegroundColor Green

# --------------------------------------------------------------- staging
$Sources = @(
    @{ From = 'mcl-ap\src\ap_modem.c';          To = 'ap_modem.c'      },
    @{ From = 'mcl-ap\include\mcl\ap_modem.h';  To = 'mcl\ap_modem.h'  },
    @{ From = 'mcl-wire\src\wire.c';            To = 'wire.c'          },
    @{ From = 'mcl-wire\include\mcl\wire.h';    To = 'mcl\wire.h'      },
    @{ From = 'mcl-link\src\link.c';            To = 'link.c'          },
    @{ From = 'mcl-link\include\mcl\link.h';    To = 'mcl\link.h'      }
)

if (Test-Path $Staged) { Remove-Item -Recurse -Force $Staged }
New-Item -ItemType Directory -Force -Path (Join-Path $Staged 'mcl') | Out-Null

Write-Host ''
Write-Host 'staged sources, by content:' -ForegroundColor Cyan
$manifest = @()
foreach ($s in $Sources) {
    $src = Join-Path $Root $s.From
    $dst = Join-Path $Staged $s.To
    if (-not (Test-Path $src)) { throw "missing source: $src" }
    Copy-Item $src $dst -Force
    $hash = (Get-FileHash $dst -Algorithm SHA256).Hash
    $manifest += [pscustomobject]@{ File = $s.From; SHA256 = $hash }
    Write-Host ("  {0,-38} {1}" -f $s.From, $hash.Substring(0, 32))
}

# --------------------------------------------------------------- compile
Write-Host ''
Write-Host "compiling for $Fqbn ..." -ForegroundColor Cyan

# -O2 rather than the core's default -Os. The acquisition inner loop is two
# multiply-accumulates per tap, run tens of millions of times per decode, and
# optimizing it for size instead of speed is worth seconds per frame.
$extra = "-O2 -I`"$Staged`""
& arduino-cli compile `
    --fqbn $Fqbn `
    --build-path $BuildPath `
    --build-property "compiler.c.extra_flags=$extra" `
    --build-property "compiler.cpp.extra_flags=$extra" `
    --warnings default `
    $Sketch
if ($LASTEXITCODE -ne 0) {
    Write-Host 'BUILD FAILED' -ForegroundColor Red
    exit 1
}

$Image = Join-Path $BuildPath 'dfr1154_mcl_node.ino.bin'
if (-not (Test-Path $Image)) { throw "no image at $Image" }
$ImageHash = (Get-FileHash $Image -Algorithm SHA256).Hash
$ImageSize = (Get-Item $Image).Length

Write-Host ''
Write-Host 'BUILD OK' -ForegroundColor Green
Write-Host ("  image      {0}" -f $Image)
Write-Host ("  size       {0} bytes" -f $ImageSize)
Write-Host ("  sha256     {0}" -f $ImageHash)

$manifestPath = Join-Path $Here 'build-manifest.txt'
$lines = @(
    "MCL embedded node firmware build",
    "built: $(Get-Date -Format 'yyyy-MM-ddTHH:mm:ssZ')",
    "fqbn:  $Fqbn",
    "",
    "STAGED SOURCES, copied from the canonical repositories at build time.",
    "The board runs the same source the host runs; these hashes say which.",
    ""
)
foreach ($m in $manifest) { $lines += ("  {0,-38} {1}" -f $m.File, $m.SHA256) }
$lines += @(
    "",
    "IMAGE",
    "",
    ("  dfr1154_mcl_node.ino.bin  {0} bytes" -f $ImageSize),
    ("  sha256 {0}" -f $ImageHash),
    "",
    "FLASH (application partition only):",
    "",
    "  esptool --chip esp32s3 --port COM3 --baud 921600 write-flash 0x20000 \",
    "    $Image",
    "",
    "Do NOT upload through the Arduino CLI: it can also rewrite the bootloader",
    "and the partition table. Restore afterwards with RESTORE_DFR1154_APP.cmd."
)
$lines | Set-Content -Path $manifestPath -Encoding utf8
Write-Host ("  manifest   {0}" -f $manifestPath)

Write-Host ''
Write-Host 'To flash the application partition ONLY:' -ForegroundColor Yellow
Write-Host "  esptool --chip esp32s3 --port COM3 --baud 921600 write-flash 0x20000 `"$Image`""
Write-Host ''
Write-Host 'Not run by this script. Flashing is a decision, not a build step.'
