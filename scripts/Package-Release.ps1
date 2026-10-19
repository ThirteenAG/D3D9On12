param(
    [Parameter(Mandatory)][string]$Build,
    [Parameter(Mandatory)][string]$Output,
    [string]$Configuration = 'Release'
)
$ErrorActionPreference = 'Stop'
$Build = (Resolve-Path -LiteralPath $Build).Path
$source = Split-Path $PSScriptRoot -Parent
$metadataPath = Join-Path $Build 'D3D9On12-build.json'
$metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
if ($metadata.releaseConfiguration -ne $true) { throw 'A production release configuration is required.' }
if ($metadata.translationLayerPatchSignature -notmatch '^[a-f0-9]{64}$') {
    throw 'Missing verified translation-layer patch signature.'
}
if ($Configuration -ne 'Release') { throw 'Release packaging requires the Release configuration.' }
$signerArch = switch ($metadata.architecture) {
    'Win32' { 'win32' }
    'x64' { 'x64' }
    default { throw "Unsupported architecture: $($metadata.architecture)" }
}
$name = "D3D9On12-$($metadata.architecture)"
$Output = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $Output) { throw "Output already exists: $Output" }
$package = Join-Path $Output $name
$licenses = Join-Path $package 'licenses'
New-Item -ItemType Directory -Path $licenses, (Join-Path $Output 'symbols') | Out-Null
Copy-Item -LiteralPath (Join-Path $Build "src/$Configuration/d3d9on12.dll") -Destination $package
Copy-Item -LiteralPath (Join-Path $Build "src/$Configuration/d3d9on12.pdb") -Destination (Join-Path $Output 'symbols')
$signerRoot = Join-Path $Build 'packages/Microsoft.Direct3D.DxbcSigner.1.0.0'
Copy-Item -LiteralPath (Join-Path $signerRoot "build/native/bin/$signerArch/dxbcSigner.dll") -Destination $package
Copy-Item -LiteralPath $metadataPath -Destination $package
Copy-Item -LiteralPath (Join-Path $source 'LICENSE') -Destination (Join-Path $licenses 'D3D9On12-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $Build 'licenses/D3D12TranslationLayer-LICENSE.txt') -Destination $licenses
Copy-Item -LiteralPath (Join-Path $Build 'licenses/DirectX-Headers-LICENSE.txt') -Destination $licenses
$signerLicenses = @(Get-ChildItem -LiteralPath $signerRoot -File -Filter 'LICENSE*')
if (!$signerLicenses.Count) { throw 'Missing DxbcSigner license.' }
$signerLicenses | Copy-Item -Destination $licenses
@'
D3D9On12

Use the architecture matching the application. Place d3d9on12.dll and
dxbcSigner.dll beside its executable. The application or loader must select
Direct3DCreate9On12 or Direct3DCreate9On12Ex and load this mapping layer.
Ordinary Direct3DCreate9 calls are not redirected by copying these files.
Keep Windows' system copies unchanged.

This is an independent fork of the Microsoft MIT-licensed project.
D3D9On12-build.json records the source and translation-layer patch revisions.
'@ | Set-Content -LiteralPath (Join-Path $package 'README.txt') -Encoding utf8
Get-ChildItem -LiteralPath $package -File | Get-FileHash -Algorithm SHA256 |
    ForEach-Object { "$($_.Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($_.Path))" } |
    Set-Content -LiteralPath (Join-Path $package 'SHA256SUMS.txt') -Encoding ascii
Compress-Archive -Path "$package/*" -DestinationPath (Join-Path $Output "$name.zip")
Compress-Archive -Path (Join-Path $Output 'symbols/d3d9on12.pdb') -DestinationPath (Join-Path $Output "$name-symbols.zip")
Get-ChildItem -LiteralPath $Output -File -Filter '*.zip' | Get-FileHash -Algorithm SHA256 |
    ForEach-Object { "$($_.Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($_.Path))" } |
    Set-Content -LiteralPath (Join-Path $Output 'SHA256SUMS.txt') -Encoding ascii
Write-Output "Packaged $name from revision $($metadata.revision)"
