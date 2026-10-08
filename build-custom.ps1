# SPDX-License-Identifier: GPL-2.0-or-later
# Modified 2026-10-08 for this independently maintained MicroSIP fork.
# Requires PowerShell 7, Git, Visual Studio 2022 C++/MFC/ATL and Windows SDK.
param(
    [string]$BuildRoot = (Join-Path (Split-Path $PSScriptRoot) 'microsip-build'),
    [switch]$SkipDependencies
)
$ErrorActionPreference = 'Stop'
$pins = Get-Content -LiteralPath "$PSScriptRoot\config\dependencies.json" -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.ATLMFC -property installationPath
if (!$vs) { throw 'Install Visual Studio 2022 C++ tools, MFC/ATL, and the Windows 10/11 SDK.' }
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
$pj = Join-Path $BuildRoot 'pjproject'
$vcpkg = Join-Path $BuildRoot 'vcpkg'
$deps = Join-Path $vcpkg 'installed\x86-windows-static'
function RunChecked([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE) { throw "$exe failed ($LASTEXITCODE)" }
}
if (!(Test-Path $pj)) {
    RunChecked git @('clone', '--depth', '1', '--branch', $pins.pjsipTag, 'https://github.com/pjsip/pjproject.git', $pj)
}
if (!(Test-Path $vcpkg)) {
    RunChecked git @('clone', '--filter=blob:none', '--no-checkout', 'https://github.com/microsoft/vcpkg.git', $vcpkg)
    RunChecked git @('-C', $vcpkg, 'checkout', $pins.vcpkgCommit)
}
function AssertRevision([string]$directory, [string]$expected) {
    if (Test-Path (Join-Path $directory '.git')) {
        $actual = & git -C $directory rev-parse HEAD
        if ($LASTEXITCODE) { throw "Cannot read dependency revision: $directory" }
    } else {
        # The corresponding-source release bundle contains Git-free archives.
        $actual = Get-Content -LiteralPath (Join-Path $directory '.microsip-source-revision') -Raw
    }
    if ($actual.Trim() -ne $expected) { throw "Unexpected dependency revision in $directory; expected $expected" }
}
AssertRevision $pj $pins.pjsipCommit
AssertRevision $vcpkg $pins.vcpkgCommit
# G.722.1 is disabled by PJSIP's default config and needs a separate Polycom
# license. Do not build its unused library; source releases omit that codec.
$aggregateProject = "$pj\pjsip-apps\build\libpjproject.vcxproj"
[xml]$aggregate = Get-Content -LiteralPath $aggregateProject -Raw
$restrictedProject = $aggregate.SelectSingleNode("//*[local-name()='ProjectReference' and contains(@Include, '\g7221\')]")
if ($restrictedProject) {
    [void]$restrictedProject.ParentNode.RemoveChild($restrictedProject)
    $aggregate.Save($aggregateProject)
}
if (!(Test-Path "$vcpkg\vcpkg.exe")) {
    RunChecked "$vcpkg\bootstrap-vcpkg.bat" @('-disableMetrics')
}
if (!$SkipDependencies) {
    RunChecked "$vcpkg\vcpkg.exe" @('install', 'opus:x86-windows-static', 'openssl:x86-windows-static', 'sqlitecpp:x86-windows-static', '--disable-metrics')
}
$pjConfig = "$pj\pjlib\include\pj\config_site.h"
$sourceConfig = "$PSScriptRoot\config\pj-config-site.h"
if (!(Test-Path $pjConfig) -or (Get-FileHash $sourceConfig).Hash -ne (Get-FileHash $pjConfig).Hash) {
    Copy-Item -LiteralPath $sourceConfig -Destination $pjConfig -Force
}
# PJSIP's legacy projects inherit compiler search paths from the environment.
$env:INCLUDE = "$deps\include;$env:INCLUDE"
$env:LIB = "$deps\lib;$env:LIB"
RunChecked $msbuild @("$pj\pjsip-apps\build\libpjproject.vcxproj", '/m:4', '/p:Configuration=Release-Static', '/p:Platform=Win32', '/p:PlatformToolset=v143', '/p:BuildToolset=v143', '/p:WindowsTargetPlatformVersion=10.0', "/p:ForceImportBeforeCppTargets=$PSScriptRoot\config\dependency-paths.props", "/p:DependencyDir=$deps", '/v:minimal', '/nologo')
RunChecked $msbuild @("$PSScriptRoot\audio\AudioGuard.vcxproj", '/m:4', '/p:Configuration=Release', '/p:Platform=Win32', '/v:minimal', '/nologo')
RunChecked $msbuild @("$PSScriptRoot\custom.vcxproj", '/m:4', '/p:Configuration=Release', '/p:Platform=Win32', "/p:PjProjectDir=$pj", "/p:DependencyDir=$deps", '/v:minimal', '/nologo')
Write-Output "Built $PSScriptRoot\out\microsip.exe"
