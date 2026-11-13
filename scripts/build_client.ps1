param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
& (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration