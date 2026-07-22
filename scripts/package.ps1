param(
    [string] $QtRoot = 'C:\msys64\ucrt64',
    [string] $Destination = (Join-Path $PSScriptRoot '..\publish')
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$executable = Join-Path $projectRoot 'bin\PBR Material Editor.exe'
$cliExecutable = Join-Path $projectRoot 'bin\pbrm.exe'

if (-not (Test-Path $executable)) {
    throw "Build the release executable first: $executable"
}
if (-not (Test-Path $cliExecutable)) {
    throw "Build the CLI companion first (cli\PbrmCli.pro): $cliExecutable"
}

New-Item -ItemType Directory -Force -Path $Destination | Out-Null
# Smoke-test captures belong with test artifacts, never in the portable product.
Get-ChildItem -LiteralPath $Destination -File -Filter '*smoke*.png' -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem -LiteralPath $Destination -File -Filter 'portable-*.png' -ErrorAction SilentlyContinue | Remove-Item -Force
Copy-Item -Force $executable $Destination
Copy-Item -Force $cliExecutable $Destination
Copy-Item -Force (Join-Path $projectRoot 'README.md') $Destination

$licenseDirectory = Join-Path $Destination 'licenses'
New-Item -ItemType Directory -Force -Path $licenseDirectory | Out-Null
Copy-Item -Force (Join-Path $projectRoot 'third_party\ufbx\LICENSE') (Join-Path $licenseDirectory 'ufbx-LICENSE.txt')
Copy-Item -Force (Join-Path $projectRoot 'third_party\detex\LICENSE.txt') (Join-Path $licenseDirectory 'detex-LICENSE.txt')
Copy-Item -Force (Join-Path $projectRoot 'third_party\tinyexr\LICENSE') (Join-Path $licenseDirectory 'tinyexr-LICENSE.txt')
Copy-Item -Force (Join-Path $projectRoot 'third_party\bc7enc\LICENSE') (Join-Path $licenseDirectory 'bc7enc-LICENSE.txt')

$deployQt = Join-Path $QtRoot 'bin\windeployqt6.exe'
& $deployQt --release --no-translations (Join-Path $Destination 'PBR Material Editor.exe')
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }

# MSYS2's windeployqt currently omits its MinGW and third-party runtime DLLs.
# Keep this explicit so portable builds do not silently depend on the developer's PATH.
$runtimeDlls = @(
    'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll',
    'libmd4c.dll', 'libpng16-16.dll', 'zlib1.dll', 'libb2-1.dll',
    'libdouble-conversion.dll', 'libicuin78.dll', 'libicuuc78.dll', 'libicudt78.dll',
    'libfreetype-6.dll', 'libharfbuzz-0.dll', 'libgraphite2.dll',
    'libpcre2-16-0.dll', 'libpcre2-8-0.dll', 'libzstd.dll',
    'libbrotlidec.dll', 'libbrotlicommon.dll', 'libbz2-1.dll',
    'libglib-2.0-0.dll', 'libintl-8.dll', 'libiconv-2.dll',
    'libdeflate.dll', 'libffi-8.dll', 'libgio-2.0-0.dll',
    'libgmodule-2.0-0.dll', 'libgobject-2.0-0.dll',
    'libjasper.dll', 'libjbig-0.dll', 'libjpeg-8.dll', 'liblcms2-2.dll',
    'libLerc.dll', 'liblzma-5.dll', 'libmng-2.dll', 'libsharpyuv-0.dll',
    'libtiff-6.dll', 'libwebp-7.dll', 'libwebpdemux-2.dll', 'libwebpmux-3.dll'
)

foreach ($dll in $runtimeDlls) {
    Copy-Item -Force (Join-Path $QtRoot "bin\$dll") $Destination
}

Write-Host "Portable build created at $Destination"
