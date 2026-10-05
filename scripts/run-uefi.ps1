param(
    [string]$OvmfCodePath = 'C:\msys64\mingw64\share\qemu\edk2-x86_64-code.fd',

    [string]$OvmfVarsPath = 'C:\msys64\mingw64\share\qemu\edk2-i386-vars.fd',

    [string]$NasmPath = 'nasm',
    [string]$QemuPath = 'qemu-system-x86_64'
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $OvmfCodePath -PathType Leaf)) {
    throw "OVMF code firmware not found: $OvmfCodePath"
}
if (-not (Test-Path -LiteralPath $OvmfVarsPath -PathType Leaf)) {
    throw "OVMF variable-store firmware not found: $OvmfVarsPath"
}

& (Join-Path $PSScriptRoot 'build-uefi.ps1') -NasmPath $NasmPath
if ($LASTEXITCODE -ne 0) {
    throw "The UEFI build failed (exit code $LASTEXITCODE)."
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$outputRoot = Join-Path $projectRoot 'out\uefi'
$espRoot = Join-Path $outputRoot 'esp'
$varsCopyPath = Join-Path $outputRoot 'OVMF_VARS.fd'

Copy-Item -Force -Path $OvmfVarsPath -Destination $varsCopyPath
& $QemuPath `
    -machine q35 `
    -m 512M `
    -drive "if=pflash,format=raw,readonly=on,file=$OvmfCodePath" `
    -drive "if=pflash,format=raw,file=$varsCopyPath" `
    -drive "format=raw,file=fat:rw:$espRoot" `
    -serial stdio `
    -no-reboot

if ($LASTEXITCODE -ne 0) {
    throw "QEMU failed to run the UEFI image (exit code $LASTEXITCODE)."
}
