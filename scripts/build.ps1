param(
    [ValidateSet('Debug','Release')][string]$Configuration='Release',
    [ValidateRange(1,64)][int]$Jobs=2
)
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$preset='windows-'+$Configuration.ToLowerInvariant()
foreach($requiredTool in @('cmake','ninja','g++')) {
    if(-not(Get-Command $requiredTool -CommandType Application -ErrorAction SilentlyContinue)) {
        throw "Missing $requiredTool in PATH. See README.md for Windows build prerequisites."
    }
}
Push-Location -LiteralPath $projectRoot
try {
    & cmake --preset $preset
    if($LASTEXITCODE -ne 0){throw 'CMake configure failed'}
    & cmake --build --preset $preset --parallel $Jobs
    if($LASTEXITCODE -ne 0){throw 'CMake build failed'}
    Write-Host "BUILD OK: $projectRoot\build\bin"
} finally {Pop-Location}
