<#
.SYNOPSIS
Zips the runtime files of one Windows build into dist/echoir-<version>-<preset>.zip.

.DESCRIPTION
Run `cmake --build --preset <Preset>` first. The archive holds echoir.exe, echoir.dll, the ggml
DLLs and any whisper, llama or mtmd DLLs (when those libraries were built shared), the Visual C++
runtime DLLs (from the Visual Studio redistributable folder), the THIRD_PARTY_NOTICES file, the licenses folder, a README.txt and models/registry.json. echoir.exe finds models\ next to
itself, so the archive runs from any folder. Model files are not included.

.PARAMETER Preset
windows-vulkan-x64 (default) or windows-cpu-x64.

.EXAMPLE
tools\package-windows.ps1 -Preset windows-cpu-x64
#>
[CmdletBinding()]
param(
    [ValidateSet('windows-vulkan-x64', 'windows-cpu-x64')]
    [string]$Preset = 'windows-vulkan-x64'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Preset\bin"

if (-not (Test-Path (Join-Path $bin 'echoir.exe'))) {
    throw "$bin\echoir.exe is missing. Build the preset first: cmake --build --preset $Preset"
}

# The release version is the project version in CMakeLists.txt.
$cmakeText = Get-Content -Raw -Path (Join-Path $root 'CMakeLists.txt')
if ($cmakeText -notmatch 'project\(echoir\s+VERSION\s+(\d+\.\d+\.\d+)') {
    throw 'Cannot read the project version from CMakeLists.txt.'
}
$version = $Matches[1]
$name = "echoir-$version-$Preset"

# Runtime files: the CLI, the C ABI library and the shared ggml, whisper, llama and mtmd libraries.
$files = @('echoir.exe', 'echoir.dll')
$files += Get-ChildItem -Path $bin -Filter '*.dll' |
    Where-Object { $_.Name -match '^(ggml|whisper|llama|mtmd)[-.\w]*\.dll$' } |
    ForEach-Object { $_.Name }
foreach ($file in $files) {
    if (-not (Test-Path (Join-Path $bin $file))) { throw "$file is missing from $bin." }
}
if (@($files | Where-Object { $_ -eq 'ggml.dll' }).Count -ne 1) {
    throw "Expected exactly one ggml.dll in $bin."
}

# The Visual C++ runtime ships next to the binaries, so the archive runs without the
# redistributable installer. It stays a shared DLL rather than static: every library then shares one
# C runtime, so environment variables set by a host process reach the core.
$redist = $env:VCToolsRedistDir
if (-not $redist) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $redist = Get-ChildItem (Join-Path $vs 'VC\Redist\MSVC') -Directory |
        Where-Object { $_.Name -match '^\d+\.' } | Sort-Object { [version]$_.Name } | Select-Object -Last 1 -ExpandProperty FullName
}
$crt = Get-ChildItem (Join-Path $redist 'x64') -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
$omp = Get-ChildItem (Join-Path $redist 'x64') -Directory -Filter 'Microsoft.VC*.OpenMP' | Select-Object -First 1
if (-not $crt -or -not $omp) { throw "The Visual C++ redistributable folder was not found under $redist." }
$runtime = @(
    (Join-Path $crt.FullName 'vcruntime140.dll'),
    (Join-Path $crt.FullName 'vcruntime140_1.dll'),
    (Join-Path $crt.FullName 'msvcp140.dll'),
    (Join-Path $omp.FullName 'vcomp140.dll'))
foreach ($file in $runtime) {
    if (-not (Test-Path $file)) { throw "$file is missing." }
}

$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
# Keeps the archives out of git without touching the root .gitignore.
Set-Content -Path (Join-Path $dist '.gitignore') -Value '*' -Encoding ascii

$stage = Join-Path ([IO.Path]::GetTempPath()) ("echoir-package-" + [guid]::NewGuid().ToString('N'))
$pkg = Join-Path $stage $name
try {
    New-Item -ItemType Directory -Force -Path (Join-Path $pkg 'models') | Out-Null
    foreach ($file in $files) {
        Copy-Item -Path (Join-Path $bin $file) -Destination $pkg
    }
    foreach ($file in $runtime) {
        Copy-Item -Path $file -Destination $pkg
    }
    Copy-Item -Path (Join-Path $root 'models\registry.json') -Destination (Join-Path $pkg 'models')
    $notices = Join-Path $root 'THIRD_PARTY_NOTICES'
    if (Test-Path $notices) { Copy-Item -Path $notices -Destination $pkg }
    $license = Join-Path $root 'LICENSE'
    if (Test-Path $license) { Copy-Item -Path $license -Destination $pkg }

    # The full third-party license texts, in licenses\. A missing source fails the build.
    $licenses = Join-Path $pkg 'licenses'
    New-Item -ItemType Directory -Force -Path $licenses | Out-Null
    $singleLicenses = @(Get-ChildItem -Path (Join-Path $root 'third_party\single') -Filter '*.LICENSE.txt')
    if ($singleLicenses.Count -eq 0) { throw 'No third_party\single\*.LICENSE.txt files were found.' }
    $licenseFiles = @(
        @{ Source = Join-Path $root 'third_party\whisper.cpp\LICENSE'; Name = 'whisper.cpp.LICENSE.txt' },
        @{ Source = Join-Path $root 'third_party\llama.cpp\LICENSE'; Name = 'llama.cpp.LICENSE.txt' })
    $licenseFiles += $singleLicenses | ForEach-Object { @{ Source = $_.FullName; Name = $_.Name } }
    $licenseFiles += Get-ChildItem -Path (Join-Path $root 'third_party\licenses') -Filter '*.txt' |
        ForEach-Object { @{ Source = $_.FullName; Name = $_.Name } }
    foreach ($entry in $licenseFiles) {
        if (-not (Test-Path $entry.Source)) { throw "License file $($entry.Source) is missing." }
        Copy-Item -Path $entry.Source -Destination (Join-Path $licenses $entry.Name)
    }

    $gpu = if ($Preset -like '*vulkan*') {
        'This build uses the Vulkan GPU backend. vulkan-1.dll comes with the GPU driver. Run with' +
        ' --no-gpu to use the CPU.'
    } else {
        'This build is CPU only.'
    }
    $readme = @"
echoir $version ($Preset)

echoir.exe              command line: transcribe, bench, score, devices, models, serve
echoir.dll              C ABI library (echoir.h)
ggml*.dll, whisper*.dll,
llama*.dll, mtmd*.dll   runtime libraries. There is one copy of each.
vcruntime140*.dll, msvcp140.dll,
vcomp140.dll            Microsoft Visual C++ runtime (redistributable files).
licenses\               full license texts of the third-party libraries (THIRD_PARTY_NOTICES
                        lists them).
models\registry.json    model catalogue. Model files go in models\ next to echoir.exe.
                        The model files are not in this archive: fetch them with
                        tools\fetch-models.ps1 from the repository, or copy them there.

Requirements
- Windows 10 or 11, x64. The Visual C++ runtime is included.
- $gpu

Quick start (from this folder)
  echoir.exe models path whisper-tiny-en
  echoir.exe transcribe <file.wav> --model whisper-tiny-en --offline
"@
    Set-Content -Path (Join-Path $pkg 'README.txt') -Value $readme -Encoding utf8

    $zip = Join-Path $dist "$name.zip"
    if (Test-Path $zip) { Remove-Item -Path $zip }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory(
        $pkg, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
}
finally {
    Remove-Item -Path $stage -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Wrote $zip"
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $archive.Entries | ForEach-Object { '{0,12}  {1}' -f $_.Length, $_.FullName }
}
finally {
    $archive.Dispose()
}
