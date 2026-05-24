$ErrorActionPreference = 'Stop'

$VERSION = "v02"

Write-Host "=== Building ArduClaw $VERSION ===" -ForegroundColor Cyan
pio run
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$buildDir = ".pio\build\esp32dev"
$outDir = "ArduClaw-flasher"

Remove-Item -Path "$outDir\*.bin" -Force -ErrorAction SilentlyContinue

# Merge bootloader + partitions + firmware into one flashable binary (for first-time flash)
$esptool = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"
$bootloader = "$buildDir\bootloader.bin"
$partitions = "$buildDir\partitions.bin"
$firmware   = "$buildDir\firmware.bin"
$merged     = "$outDir\arduclaw-esp32-$VERSION.bin"

Write-Host "=== Merging firmware ===" -ForegroundColor Cyan
& python $esptool --chip esp32 merge_bin --output $merged `
  0x1000 $bootloader `
  0x8000 $partitions `
  0x10000 $firmware

if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# firmware.js uses the merged binary (bootloader + partitions + app)
Write-Host "=== Generating firmware.js ===" -ForegroundColor Cyan
$bytes = [System.IO.File]::ReadAllBytes($merged)
$b64 = [Convert]::ToBase64String($bytes)
$js = "var FIRMWARE_DATA = { name: 'arduclaw-esp32-$VERSION.bin', size: $($bytes.Length), b64: '$b64' };"
Set-Content -Path "$outDir\firmware.js" -Value $js

Write-Host "=== Done ===" -ForegroundColor Green
Write-Host "Merged: arduclaw-esp32-$VERSION.bin ($((Get-Item $merged).Length) bytes)"
Write-Host "firmware.js: merged binary ($($bytes.Length) bytes) - flashes at 0x0"
