# SPDX-License-Identifier: GPL-2.0-or-later
# This fixed helper is embedded in the application. No script from a downloaded
# release is executed. Requires Windows PowerShell 5.1 / .NET Framework 4.5+.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('Prepare', 'Apply')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$StagePath,
    [Parameter(Mandatory = $true)][string]$ExpectedVersion,
    [Parameter(Mandatory = $true)][string]$ArchiveSha256,
    [string]$InstallPath,
    [string]$ProfileDirectory,
    [int]$ParentProcessId = 0,
    [long]$ParentStartedUtcFileTime = 0,
    [switch]$NonInteractive
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$script:Stage = $null
$script:DidExit = $false
$script:DidCommit = $false
$script:Install = $null
$script:Journal = New-Object 'System.Collections.Generic.List[object]'
$script:Backup = $null

function Assert-NormalPath([string]$Path) {
    # Reject junctions/symlinks in every existing component, including ancestors.
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Reparse points are not permitted in update paths: $cursor"
            }
        }
        $parent = [IO.Path]::GetDirectoryName($cursor.TrimEnd('\'))
        if (!$parent -or $parent -eq $cursor) { break }
        $cursor = $parent
    }
}

function Get-DirectoryPath([string]$Path) {
    if (!$Path -or ![IO.Path]::IsPathRooted($Path)) { throw 'Update paths must be absolute.' }
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    if ($full.Length -le [IO.Path]::GetPathRoot($full).Length) { throw 'A drive root cannot be an update directory.' }
    Assert-NormalPath $full
    if (!(Test-Path -LiteralPath $full -PathType Container)) { throw "Update directory does not exist: $full" }
    return $full
}

function Join-Contained([string]$Root, [string]$Relative) {
    $full = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if (!$full.StartsWith($Root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "An update path escapes its directory: $Relative"
    }
    Assert-NormalPath $full
    return $full
}

function Get-Sha256([string]$Path) {
    Assert-NormalPath $Path
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}

function Normalize-Entry([string]$Name) {
    if (!$Name -or $Name.Length -gt 1024 -or $Name.Contains('//') -or $Name.Contains('\') -or $Name.StartsWith('/') -or $Name.Contains(':')) {
        throw "Invalid archive path: $Name"
    }
    $parts = $Name.TrimEnd('/').Split('/')
    foreach ($part in $parts) {
        if (!$part -or $part.Length -gt 128 -or $part -eq '.' -or $part -eq '..' -or $part -notmatch '^[A-Za-z0-9_. -]+$' -or
            $part.EndsWith('.') -or $part.EndsWith(' ') -or
            $part -match '^(?i:CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\.|$)') {
            throw "Unsafe archive path component: $Name"
        }
    }
    if ($parts[0] -cne 'MicroSIP-Custom') { throw 'The archive has an unexpected root directory.' }
    return ($parts -join '/')
}

function Is-ApplicationFile([string]$Relative) {
    # Configuration, history, recordings, custom assets and even stock sounds
    # are deliberately excluded from the replacement list.
    if ($Relative -cin @('microsip.exe', 'MicroSIPAudioGuard.exe', 'CUSTOM-BUILD.txt', 'README.md',
        'LICENSE', 'COPYING', 'THIRD-PARTY-NOTICES.md', 'SOURCE-OFFER.txt', 'UPSTREAM-SOURCE.md',
        'BUILD-REVISION.txt', 'SHA256SUMS.txt')) { return $true }
    if ($Relative.StartsWith('licenses/', [StringComparison]::Ordinal) -and
        $Relative -notmatch '(?i)\.(exe|dll|com|bat|cmd|ps1|psm1|vbs|js|lnk|url|scr|msi)$') { return $true }
    return $false
}

function Read-Checksums([string]$Path) {
    $info = Get-Item -LiteralPath $Path -Force
    if ($info.Length -gt 262144) { throw 'The internal checksum manifest is too large.' }
    $checksums = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($line in [IO.File]::ReadAllLines($Path)) {
        if (!$line) { continue }
        if ($line -notmatch '^([a-fA-F0-9]{64}) [ *](.+)$') { throw 'The internal checksum manifest has an invalid entry.' }
        $digest = $Matches[1]
        $name = $Matches[2]
        if ($name -eq 'SHA256SUMS.txt') { throw 'The checksum manifest cannot hash itself.' }
        $normalized = Normalize-Entry ('MicroSIP-Custom/' + $name)
        if ($normalized.Substring(16) -cne $name -or $checksums.ContainsKey($name)) {
            throw 'The checksum manifest has an ambiguous or duplicate path.'
        }
        $checksums.Add($name, $digest.ToLowerInvariant())
    }
    return ,$checksums
}

function Prepare-Payload {
    if ($ExpectedVersion -notmatch '^\d{1,5}\.\d{1,5}\.\d{1,5}\.\d{1,5}$' -or
        @($ExpectedVersion.Split('.') | Where-Object { [int]$_ -gt 65535 }).Count) {
        throw 'The expected release version is invalid.'
    }
    if ($ArchiveSha256 -notmatch '^[a-fA-F0-9]{64}$') { throw 'The expected archive checksum is invalid.' }
    $archive = Join-Contained $script:Stage 'release.zip'
    $archiveInfo = Get-Item -LiteralPath $archive -Force
    if ($archiveInfo.Length -le 0 -or $archiveInfo.Length -gt 134217728) { throw 'The archive has an invalid size.' }
    if ((Get-Sha256 $archive) -ine $ArchiveSha256) {
        throw 'The downloaded archive failed SHA256 verification.'
    }
    $payload = Join-Contained $script:Stage ('payload-' + [guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($payload)
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $files = New-Object 'System.Collections.Generic.List[string]'
    $total = [long]0
    $actualTotal = [long]0
    $buffer = New-Object byte[] 32768
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        if ($zip.Entries.Count -gt 2048) { throw 'The archive has too many entries.' }
        foreach ($entry in $zip.Entries) {
            $normalized = Normalize-Entry $entry.FullName
            if (!$seen.Add($normalized)) { throw "Duplicate archive path: $normalized" }
            $unixType = ($entry.ExternalAttributes -shr 16) -band 0xF000
            if ($unixType -eq 0xA000 -or ($entry.ExternalAttributes -band 0x400)) {
                throw 'Archive symlinks and reparse points are not permitted.'
            }
            $directory = $entry.FullName.EndsWith('/')
            if ($normalized -ceq 'MicroSIP-Custom') {
                if (!$directory) { throw 'The package root must be a directory.' }
                continue
            }
            $relative = $normalized.Substring(16)
            $destination = Join-Contained $payload $relative.Replace('/', '\')
            if ($directory) {
                [void][IO.Directory]::CreateDirectory($destination)
                continue
            }
            if ($entry.Length -gt 134217728) { throw 'An archive entry is too large.' }
            $total += $entry.Length
            if ($total -gt 268435456) { throw 'The expanded archive exceeds its allowed size.' }
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
            $entryInput = $entry.Open()
            $entryOutput = [IO.File]::Open($destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
            try {
                $written = [long]0
                while (($received = $entryInput.Read($buffer, 0, $buffer.Length)) -gt 0) {
                    if ($received -gt $entry.Length - $written -or $received -gt 134217728 - $written -or
                        $received -gt 268435456 - $actualTotal) { throw 'Archive output exceeds its declared or allowed size.' }
                    $entryOutput.Write($buffer, 0, $received)
                    $written += $received
                    $actualTotal += $received
                }
                if ($written -ne $entry.Length) { throw 'An archive entry was truncated.' }
            } finally { $entryOutput.Dispose(); $entryInput.Dispose() }
            if ((Get-Item -LiteralPath $destination).Length -ne $entry.Length) { throw 'An archive entry was truncated.' }
            $files.Add($relative)
        }
    } finally { $zip.Dispose() }
    foreach ($required in @('microsip.exe', 'MicroSIPAudioGuard.exe', 'LICENSE', 'COPYING',
        'THIRD-PARTY-NOTICES.md', 'BUILD-REVISION.txt', 'SHA256SUMS.txt')) {
        if (!$files.Contains($required)) { throw "The package is missing $required" }
    }
    $checksums = Read-Checksums (Join-Contained $payload 'SHA256SUMS.txt')
    foreach ($file in $files) {
        if ($file -ceq 'SHA256SUMS.txt') { continue }
        if (!$checksums.ContainsKey($file)) { throw "The package has an unhashed file: $file" }
        if ((Get-Sha256 (Join-Contained $payload $file.Replace('/', '\'))) -ine $checksums[$file]) {
            throw "Package file failed SHA256 verification: $file"
        }
    }
    if ($checksums.Count -ne $files.Count - 1) { throw 'The checksum manifest names missing package files.' }
    $binary = Join-Contained $payload 'microsip.exe'
    $actual = [Diagnostics.FileVersionInfo]::GetVersionInfo($binary).FileVersion
    if (!$actual -or [version]$actual -ne [version]$ExpectedVersion) { throw 'The executable version does not match the release.' }
    $revision = [IO.File]::ReadAllLines((Join-Contained $payload 'BUILD-REVISION.txt'))
    if ($revision.Count -lt 2 -or $revision[0] -notmatch '^[a-fA-F0-9]{40}$' -or
        $revision[1] -cne $ExpectedVersion) { throw 'The package revision does not match the release.' }
    return @{ Root = $payload; Files = @($files | Where-Object { Is-ApplicationFile $_ }); Hashes = $checksums }
}

function Wait-Parent {
    if ($ParentProcessId -le 0 -or $ParentStartedUtcFileTime -le 0) { throw 'The update needs the original application process identity.' }
    $parent = Get-Process -Id $ParentProcessId -ErrorAction SilentlyContinue
    if ($parent) {
        if ($parent.StartTime.ToUniversalTime().ToFileTimeUtc() -ne $ParentStartedUtcFileTime) {
            throw 'The original process ID has been reused; no files were changed.'
        }
        if ($parent.Path -ine (Join-Contained $script:Install 'microsip.exe')) {
            throw 'The update parent is not the application being replaced.'
        }
        if (!$parent.WaitForExit(300000)) { throw 'The application did not exit. No update was applied.' }
    }
    $script:DidExit = $true
}

function Copy-Retry([string]$Source, [string]$Destination) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ($true) {
        try {
            Assert-NormalPath $Source
            Assert-NormalPath $Destination
            [IO.File]::Copy($Source, $Destination, $true)
            return
        } catch [IO.IOException] {
            if ([DateTime]::UtcNow -ge $deadline) { throw }
            Start-Sleep -Milliseconds 250
        }
    }
}

function Restart-Application {
    $binary = Join-Contained $script:Install 'microsip.exe'
    if (!(Test-Path -LiteralPath $binary -PathType Leaf)) { throw 'The application could not be restarted; its executable is missing.' }
    if ($ProfileDirectory) {
        # Start-Process joins ArgumentList into a native command line. Quote the
        # path literally and double trailing backslashes for CommandLineToArgvW.
        # No selected profile path is evaluated as PowerShell source.
        $profileArgument = '"' + [regex]::Replace($ProfileDirectory, '(\\+)$', '$1$1') + '"'
        Start-Process -FilePath $binary -WorkingDirectory $script:Install -WindowStyle Hidden `
            -ArgumentList ('/profile-directory ' + $profileArgument) | Out-Null
    } else {
        Start-Process -FilePath $binary -WorkingDirectory $script:Install -WindowStyle Hidden | Out-Null
    }
}

function Rollback-Update {
    $errors = New-Object 'System.Collections.Generic.List[string]'
    for ($i = $script:Journal.Count - 1; $i -ge 0; $i--) {
        $entry = $script:Journal[$i]
        try {
            $destination = Join-Contained $script:Install $entry.Name.Replace('/', '\')
            if ($entry.Existed) {
                $saved = Join-Contained $script:Backup $entry.Name.Replace('/', '\')
                # A locked file may have rejected replacement without changing.
                # Do not require a redundant write to that still-locked file.
                if (!(Test-Path -LiteralPath $destination -PathType Leaf) -or
                    (Get-Sha256 $destination) -ine (Get-Sha256 $saved)) { Copy-Retry $saved $destination }
            } elseif (Test-Path -LiteralPath $destination -PathType Leaf) {
                # Delete only a newly-created allowlisted application file.
                Assert-NormalPath $destination
                [IO.File]::Delete($destination)
            }
        } catch { $errors.Add($_.Exception.Message) }
    }
    if ($errors.Count) { throw ('Rollback needs manual recovery from ' + $script:Backup + ': ' + ($errors -join '; ')) }
}

try {
    $script:Stage = Get-DirectoryPath $StagePath
    if ($ProfileDirectory) {
        if (![IO.Path]::IsPathRooted($ProfileDirectory) -or $ProfileDirectory -match '[\x00-\x1f"]') {
            throw 'The restart profile directory must be an absolute Windows path.'
        }
        $ProfileDirectory = [IO.Path]::GetFullPath($ProfileDirectory)
    }
    if ($Mode -eq 'Prepare') {
        $payload = Prepare-Payload
        [IO.File]::WriteAllText((Join-Contained $script:Stage 'prepared.json'),
            (@{ Version = $ExpectedVersion; ArchiveSHA256 = $ArchiveSha256 } | ConvertTo-Json), [Text.Encoding]::UTF8)
        exit 0
    }
    $script:Install = Get-DirectoryPath $InstallPath
    if ($script:Stage.StartsWith($script:Install + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $script:Install.StartsWith($script:Stage + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $script:Stage -ieq $script:Install) { throw 'Install and staging directories must be separate.' }
    $installedBinary = Join-Contained $script:Install 'microsip.exe'
    if (!(Test-Path -LiteralPath $installedBinary -PathType Leaf)) { throw 'The installation has no microsip.exe.' }
    Wait-Parent
    # Stage() already validated the archive while the application was running.
    # Revalidate after exit as well; failure now restarts the unchanged old app.
    $payload = Prepare-Payload
    $script:Backup = Join-Contained $script:Stage ('backup-' + [guid]::NewGuid().ToString('N'))
    [void][IO.Directory]::CreateDirectory($script:Backup)
    # Back up every destination before replacing any file. Existing app data
    # never enters this journal and is never copied, deleted or overwritten.
    foreach ($file in $payload.Files) {
        $destination = Join-Contained $script:Install $file.Replace('/', '\')
        $existed = Test-Path -LiteralPath $destination -PathType Leaf
        if ((Test-Path -LiteralPath $destination) -and !$existed) { throw "An application file is a directory: $file" }
        if ($existed) {
            $backupFile = Join-Contained $script:Backup $file.Replace('/', '\')
            [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($backupFile))
            Copy-Retry $destination $backupFile
        }
    }
    foreach ($file in $payload.Files) {
        $destination = Join-Contained $script:Install $file.Replace('/', '\')
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
        $entry = @{ Name = $file; Existed = Test-Path -LiteralPath $destination -PathType Leaf }
        $script:Journal.Add($entry)
        $source = Join-Contained $payload.Root $file.Replace('/', '\')
        Copy-Retry $source $destination
        $expectedHash = if ($file -ceq 'SHA256SUMS.txt') { Get-Sha256 $source } else { $payload.Hashes[$file] }
        if ((Get-Sha256 $destination) -ine $expectedHash) {
            throw "Installed file failed SHA256 verification: $file"
        }
    }
    $script:DidCommit = $true
    [IO.File]::WriteAllText((Join-Contained $script:Stage 'applied.json'),
        (@{ Version = $ExpectedVersion; Backup = $script:Backup } | ConvertTo-Json), [Text.Encoding]::UTF8)
    Restart-Application
    exit 0
} catch {
    $message = $_.Exception.Message
    if ($script:Journal.Count -gt 0 -and !$script:DidCommit) {
        try { Rollback-Update } catch { $message += "`r`n" + $_.Exception.Message }
    }
    if ($script:Stage) {
        try { [IO.File]::WriteAllText((Join-Contained $script:Stage 'update-error.txt'), $message, [Text.UTF8Encoding]::new($false)) } catch {}
    }
    if ($script:DidExit -and $script:Install) {
        try { Restart-Application } catch { $message += "`r`n" + $_.Exception.Message }
    }
    if ($Mode -eq 'Apply' -and !$NonInteractive) {
        try {
            Add-Type -AssemblyName System.Windows.Forms
            [void][Windows.Forms.MessageBox]::Show("The MicroSIP Custom update could not be completed.`r`n`r`n$message`r`n`r`nDetails: $script:Stage",
                'MicroSIP Custom update', [Windows.Forms.MessageBoxButtons]::OK, [Windows.Forms.MessageBoxIcon]::Error)
        } catch {}
    }
    Write-Error -Message $message -ErrorAction Continue
    exit 1
}
