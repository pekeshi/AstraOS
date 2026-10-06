param(
    [string]$OvmfCodePath = 'C:\msys64\mingw64\share\qemu\edk2-x86_64-code.fd',

    [string]$OvmfVarsPath = 'C:\msys64\mingw64\share\qemu\edk2-i386-vars.fd',

    [string]$NasmPath = 'nasm',
    [string]$QemuPath = 'qemu-system-x86_64'
)

$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$oldPath = $env:PATH
try {
    if ($NasmPath -ne 'nasm') {
        $nasmCommand = Get-Command $NasmPath -ErrorAction Stop
        $nasmDirectory = Split-Path -Parent $nasmCommand.Source
        if ($nasmDirectory) {
            $env:PATH = "$nasmDirectory;$env:PATH"
        }
    }
    Push-Location $projectRoot
    try {
        & $env:ComSpec /c 'build.bat'
        $buildExitCode = $LASTEXITCODE
    } finally {
        Pop-Location
    }
} finally {
    $env:PATH = $oldPath
}
if ($buildExitCode -ne 0) {
    throw "The UEFI build failed (exit code $buildExitCode)."
}

if (-not (Test-Path -LiteralPath $OvmfCodePath -PathType Leaf)) {
    throw "OVMF code firmware not found: $OvmfCodePath"
}
if (-not (Test-Path -LiteralPath $OvmfVarsPath -PathType Leaf)) {
    throw "OVMF variable-store firmware not found: $OvmfVarsPath"
}

$outputRoot = Join-Path $projectRoot 'out\uefi'
$espRoot = Join-Path $outputRoot 'esp'
$varsCopyPath = Join-Path $outputRoot 'OVMF_VARS.fd'

Copy-Item -Force -Path $OvmfVarsPath -Destination $varsCopyPath
& $QemuPath `
    -machine q35,i8042=off `
    -m 512M `
    -device qemu-xhci `
    -device usb-kbd `
    -drive "if=pflash,format=raw,readonly=on,file=$OvmfCodePath" `
    -drive "if=pflash,format=raw,file=$varsCopyPath" `
    -drive "format=raw,file=fat:rw:$espRoot" `
    -serial stdio `
    -no-reboot

if ($LASTEXITCODE -ne 0) {
    throw "QEMU failed to run the UEFI image (exit code $LASTEXITCODE)."
}
