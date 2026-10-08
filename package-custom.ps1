# SPDX-License-Identifier: GPL-2.0-or-later
# Modified 2026-10-08: allowlisted release staging and corresponding sources.
# Never packages an existing portable installation or copies its settings.
# Requires PowerShell 7 and Git. Build and commit the release before packaging.
param(
    [string]$BuildRoot = (Join-Path (Split-Path $PSScriptRoot) 'microsip-build'),
    [string]$SourceRevision = 'HEAD'
)
$ErrorActionPreference = 'Stop'
$pins = Get-Content -LiteralPath "$PSScriptRoot\config\dependencies.json" -Raw | ConvertFrom-Json
$version = (Get-Item -LiteralPath "$PSScriptRoot\out\microsip.exe").VersionInfo.FileVersion
if ($version -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw "Unexpected executable version: $version" }
function RunChecked([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE) { throw "$exe failed ($LASTEXITCODE)" }
}
$revision = & git -C $PSScriptRoot rev-parse $SourceRevision
if ($LASTEXITCODE -or $revision -notmatch '^[0-9a-f]{40}$') { throw 'Cannot resolve source revision.' }
$head = & git -C $PSScriptRoot rev-parse HEAD
if ($head -ne $revision) { throw 'Package the checked-out source revision used for the build.' }
$dirty = & git -C $PSScriptRoot status --porcelain --untracked-files=normal
if ($LASTEXITCODE -or $dirty) { throw 'Commit the reviewed release source before packaging.' }

# A unique fresh directory prevents account files, call history, logs and
# recordings in dist/MicroSIP-Custom from entering a public release archive.
$stage = Join-Path "$PSScriptRoot\build" ("release-stage-" + [guid]::NewGuid().ToString('N'))
$package = Join-Path $stage 'MicroSIP-Custom'
$sourceStage = Join-Path $stage 'source'
$dependencyStage = Join-Path $stage 'dependency-sources'
$dist = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Force -Path $package, "$package\licenses", $sourceStage, "$dependencyStage\downloads", $dist | Out-Null
foreach ($file in @('microsip.exe', 'MicroSIPAudioGuard.exe')) {
    Copy-Item -LiteralPath "$PSScriptRoot\out\$file" -Destination $package
}
foreach ($file in @('CUSTOM-BUILD.txt', 'README.md', 'LICENSE', 'COPYING', 'THIRD-PARTY-NOTICES.md', 'SOURCE-OFFER.txt', 'UPSTREAM-SOURCE.md')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $package
}
Copy-Item -LiteralPath "$PSScriptRoot\licenses" -Destination $package -Recurse -Force

function FetchVerified([object]$artifact, [string]$directory, [string]$algorithm = 'SHA256') {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $path = Join-Path $directory $artifact.filename
    if (!(Test-Path -LiteralPath $path)) { Invoke-WebRequest -Uri $artifact.url -OutFile $path }
    $expected = $artifact.($algorithm.ToLower())
    if ((Get-FileHash -LiteralPath $path -Algorithm $algorithm).Hash -ne $expected) {
        throw "Checksum mismatch for $path"
    }
    return $path
}
$soundArchive = FetchVerified $pins.portableSounds "$BuildRoot\downloads"
$sounds = Join-Path $stage 'official-sounds'
Expand-Archive -LiteralPath $soundArchive -DestinationPath $sounds
# WAV is the preferred source form of these assets. Include the exact sounds
# in both the runnable package and corresponding source archive.
$wavFiles = @(Get-ChildItem -LiteralPath $sounds -File -Filter '*.wav')
if (!$wavFiles.Count) { throw 'Official sound archive contained no WAV files.' }
$wavFiles | Copy-Item -Destination $package

$pj = Join-Path $BuildRoot 'pjproject'
$vcpkg = Join-Path $BuildRoot 'vcpkg'
Copy-Item -LiteralPath "$pj\COPYING" -Destination "$package\licenses\PJSIP.txt"
foreach ($dependency in @('openssl', 'opus', 'sqlite3', 'SQLiteCpp')) {
    Copy-Item -LiteralPath "$vcpkg\installed\x86-windows-static\share\$dependency\copyright" -Destination "$package\licenses\$dependency.txt"
}
foreach ($notice in Get-ChildItem -LiteralPath "$pj\third_party" -Recurse -File) {
    if ($notice.Name -notmatch '(?i)^(COPYING|LICENSE|COPYRIGHT|PATENTS)' -and $notice.Name -ne 'PJSIP_NOTES') { continue }
    if ($notice.FullName -like '*\g7221\*') { continue }
    $relative = [IO.Path]::GetRelativePath($pj, $notice.FullName)
    $destination = Join-Path "$package\licenses\pjsip" $relative
    New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null
    Copy-Item -LiteralPath $notice.FullName -Destination $destination
}
# This file is generated only from literal, account-free defaults.
$defaults = @'
[Global]
version=3.22.16
[Settings]
callAudioMode=0
callAudioApps=
updatesInterval=never
recordingFormat=wav
'@
Set-Content -LiteralPath "$package\microsip.ini" -Value $defaults -Encoding Unicode
Set-Content -LiteralPath "$package\BUILD-REVISION.txt" -Value "$revision`n$version" -Encoding ascii

function WriteChecksums([string]$directory) {
    $hashes = Get-ChildItem -LiteralPath $directory -Recurse -File |
        Where-Object Name -ne 'SHA256SUMS.txt' | Sort-Object FullName | ForEach-Object {
            $relative = [IO.Path]::GetRelativePath($directory, $_.FullName).Replace('\', '/')
            "$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower())  $relative"
        }
    Set-Content -LiteralPath "$directory\SHA256SUMS.txt" -Value $hashes -Encoding ascii
}
WriteChecksums $package
$binaryZip = "$dist\MicroSIP-Custom-$version.zip"
Compress-Archive -LiteralPath $package -DestinationPath $binaryZip -Force

$sourceArchive = Join-Path $stage 'application-source.zip'
RunChecked git @('-C', $PSScriptRoot, 'archive', '--format=zip', "--output=$sourceArchive", '--prefix=MicroSIP-Custom-source/', $revision)
Expand-Archive -LiteralPath $sourceArchive -DestinationPath $sourceStage
$sourceRoot = Join-Path $sourceStage 'MicroSIP-Custom-source'
New-Item -ItemType Directory -Path "$sourceRoot\sounds" | Out-Null
$wavFiles | Copy-Item -Destination "$sourceRoot\sounds"
Set-Content -LiteralPath "$sourceRoot\BUILD-REVISION.txt" -Value "$revision`n$version" -Encoding ascii
$sourceZip = "$dist\MicroSIP-Custom-$version-source.zip"
Compress-Archive -LiteralPath $sourceRoot -DestinationPath $sourceZip -Force

Add-Type -AssemblyName System.IO.Compression.FileSystem
function ArchiveDependency([string]$repository, [string]$commit, [string]$filename, [string]$omitPrefix = '') {
    $archive = Join-Path $dependencyStage $filename
    RunChecked git @('-C', $repository, 'archive', '--format=zip', "--output=$archive", $commit)
    $zip = [IO.Compression.ZipFile]::Open($archive, [IO.Compression.ZipArchiveMode]::Update)
    try {
        if ($omitPrefix) {
            @($zip.Entries | Where-Object { $_.FullName.StartsWith($omitPrefix) }) | ForEach-Object { $_.Delete() }
        }
        $marker = $zip.CreateEntry('.microsip-source-revision')
        $writer = [IO.StreamWriter]::new($marker.Open())
        try { $writer.WriteLine($commit) } finally { $writer.Dispose() }
    } finally { $zip.Dispose() }
}
ArchiveDependency $pj $pins.pjsipCommit 'pjproject-source.zip' 'third_party/g7221/'
ArchiveDependency $vcpkg $pins.vcpkgCommit 'vcpkg-source.zip'
foreach ($artifact in $pins.sourceArchives) {
    $archive = FetchVerified $artifact "$vcpkg\downloads" 'SHA512'
    Copy-Item -LiteralPath $archive -Destination "$dependencyStage\downloads"
}
$canonicalSqlite = FetchVerified $pins.sqliteCanonicalSource "$BuildRoot\downloads"
Copy-Item -LiteralPath $canonicalSqlite -Destination $dependencyStage
Copy-Item -LiteralPath "$PSScriptRoot\config\dependencies.json" -Destination $dependencyStage
Copy-Item -LiteralPath "$PSScriptRoot\DEPENDENCY-SOURCES.txt" -Destination "$dependencyStage\README.txt"
Copy-Item -LiteralPath "$PSScriptRoot\COPYING", "$PSScriptRoot\LICENSE" -Destination $dependencyStage
WriteChecksums $dependencyStage
$dependencyZip = "$dist\MicroSIP-Custom-$version-dependency-sources.zip"
Compress-Archive -LiteralPath $dependencyStage -DestinationPath $dependencyZip -Force
$releaseHashes = @($binaryZip, $sourceZip, $dependencyZip) | ForEach-Object {
    "$((Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLower())  $([IO.Path]::GetFileName($_))"
}
Set-Content -LiteralPath "$dist\MicroSIP-Custom-$version-SHA256SUMS.txt" -Value $releaseHashes -Encoding ascii
Write-Output "Release packages created from $revision in $dist"
Write-Output "Fresh portable staging folder: $package"
