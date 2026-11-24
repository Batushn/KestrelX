param(
    [string]$RgaExe = "D:\_downloads\rga\rga.exe"
)

$ErrorActionPreference = "Stop"
$KernelDir = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not (Test-Path $RgaExe)) {
    throw "AMD Radeon GPU Analyzer (rga.exe) not found at '$RgaExe'. Pass -RgaExe <path>."
}

$ClFiles = Get-ChildItem -Path $KernelDir -Filter "k*.cl" | Sort-Object Name
foreach ($cl in $ClFiles) {
    $name = $cl.BaseName
    $elfOut = Join-Path $KernelDir "gfx1030_${name}.elf"
    Write-Host "[RGA gfx1030] Compiling $($cl.Name) -> $(Split-Path -Leaf $elfOut)"
    & $RgaExe -s opencl -c gfx1030 -b $elfOut $cl.FullName
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $elfOut)) {
        throw "RGA compilation failed for $($cl.Name)"
    }
}
Write-Host "Successfully built $($ClFiles.Count) external gfx1030 ELF64-AMDGPU kernels."
