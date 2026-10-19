param(
    [Parameter(Mandatory)][string]$Build,
    [string]$Configuration = 'Release',
    [string]$Driver = '',
    [string]$Signer = ''
)
$ErrorActionPreference = 'Stop'
if (Get-Process GTAIV, EFLC -ErrorAction SilentlyContinue) {
    throw 'Close the game before running the GPU probes.'
}
$Build = (Resolve-Path -LiteralPath $Build).Path
$probeDirectory = Join-Path $Build "tests/$Configuration"
if (!$Driver) { $Driver = Join-Path $Build "src/$Configuration/d3d9on12.dll" }
$Driver = (Resolve-Path -LiteralPath $Driver).Path
# Select the signer from the driver's actual PE machine, independent of the
# architecture of the shell running this script.
$reader = [IO.BinaryReader]::new([IO.File]::OpenRead($Driver))
try {
    $reader.BaseStream.Position = 0x3c
    $peOffset = $reader.ReadInt32()
    $reader.BaseStream.Position = $peOffset + 4
    $machine = $reader.ReadUInt16()
} finally { $reader.Dispose() }
$architecture = switch ($machine) { 0x14c { 'win32' } 0x8664 { 'x64' } default { throw "Unsupported PE machine: $machine" } }
if (!$Signer) {
    $candidates = @(Get-ChildItem -Path (Join-Path $Build "packages/Microsoft.Direct3D.DxbcSigner.*/build/native/bin/$architecture/dxbcSigner.dll"))
    if ($candidates.Count -ne 1) { throw 'Specify -Signer with the matching dxbcSigner.dll.' }
    $Signer = $candidates[0].FullName
}
Copy-Item -LiteralPath $Driver -Destination (Join-Path $probeDirectory 'd3d9on12.dll') -Force
Copy-Item -LiteralPath $Signer -Destination (Join-Path $probeDirectory 'dxbcSigner.dll') -Force
Write-Output "driver=$Driver architecture=$architecture SHA256=$((Get-FileHash -LiteralPath $Driver).Hash)"
$cases = @(
    @('binding_state'),
    @('upload_pool'),
    @('allocation_info'),
    @('latency', '--native'), @('latency'), @('latency', '--debug'),
    @('runtime', '--require-local', '--require-extensions', '--reset'),
    @('hdr_output'),
    @('sampler'), @('sampler', '--debug', '--zero-address'),
    @('sampler', '--debug', '--binding-runs'),
    @('sampler', '--large-upload-offsets'), @('sampler', '--debug', '--large-upload-offsets'),
    @('multistream', '--on12', '--stream1'), @('multistream', '--on12', '--stream2'),
    @('query_order'), @('query_order', '--debug'),
    @('multithread_reset'), @('multithread_reset', '--debug'),
    @('per_draw', '--native'), @('per_draw'), @('per_draw', '--debug'),
    @('per_draw', '--native', '--stress-descriptors'),
    @('per_draw', '--debug', '--stress-descriptors'),
    @('multistream', '--stream15', '--rebind'),
    @('multistream', '--on12', '--stream15', '--rebind'),
    @('multistream', '--on12', '--stream15', '--managed'),
    @('multistream', '--on12', '--stream15', '--default'),
    @('multistream', '--stream15', '--managed', '--views'),
    @('multistream', '--on12', '--stream15', '--views'),
    @('multistream', '--on12', '--stream15', '--managed', '--views'),
    @('multistream', '--on12', '--stream15', '--default', '--views'),
    @('multistream', '--stream15', '--managed', '--shared-buffer'),
    @('multistream', '--on12', '--stream15', '--managed', '--shared-buffer'),
    @('multistream', '--on12', '--stream15', '--default', '--shared-buffer'),
    @('multistream', '--on12', '--stream15', '--shared-buffer'),
    @('multistream', '--stream15', '--default', '--shared-buffer', '--write-only'),
    @('multistream', '--on12', '--stream15', '--default', '--shared-buffer', '--write-only', '--debug')
)
$failures = 0
foreach ($case in $cases) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = Join-Path $probeDirectory "d3d9on12_$($case[0])_probe.exe"
    $start.WorkingDirectory = $probeDirectory
    if ($case.Count -gt 1) { $start.Arguments = $case[1..($case.Count - 1)] -join ' ' }
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $child = [Diagnostics.Process]::Start($start)
    try {
        $stdout = $child.StandardOutput.ReadToEndAsync()
        $stderr = $child.StandardError.ReadToEndAsync()
        if (!$child.WaitForExit(120000)) {
            $child.Kill()
            $child.WaitForExit()
            ++$failures
            Write-Output "TIMEOUT $($case -join ' ')"
        } elseif ($child.ExitCode) {
            ++$failures
            Write-Output "FAIL $($case -join ' ') exit=$($child.ExitCode)"
            Write-Output $stdout.Result
            Write-Output $stderr.Result
        } else {
            Write-Output "PASS $($case -join ' ')"
        }
    } finally { $child.Dispose() }
}
Write-Output "cases=$($cases.Count) failures=$failures"
exit $failures
