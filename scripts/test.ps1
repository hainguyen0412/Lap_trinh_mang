param(
    [ValidateSet('Debug','Release')][string]$Configuration='Release',
    [switch]$Build,
    [switch]$UnitOnly
)
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$preset='windows-'+$Configuration.ToLowerInvariant()
Push-Location -LiteralPath $projectRoot
try {
    if($Build){& "$PSScriptRoot\build.ps1" -Configuration $Configuration}
    if(-not(Test-Path -LiteralPath "build\$preset\CTestTestfile.cmake")) {
        throw 'CTest configuration missing. Run scripts/build.ps1 first, or use test.ps1 -Build.'
    }
    $testArguments=@('--preset',$preset,'--output-on-failure')
    if($UnitOnly){$testArguments+=@('-L','unit')}
    & ctest @testArguments
    if($LASTEXITCODE -ne 0){throw 'Tests failed. See CTest output and output/test_results.'}
} finally {Pop-Location}
