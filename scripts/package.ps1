param([switch]$WithBinaries,[string]$OutputDirectory='build/releases')
$ErrorActionPreference='Stop'
$projectRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location -LiteralPath $projectRoot
try {
    $packageArguments=@('-B','tools/package_release.py','--output-dir',$OutputDirectory)
    if($WithBinaries){$packageArguments+='--with-binaries'}
    & python @packageArguments
    if($LASTEXITCODE -ne 0){throw 'Packaging failed'}
} finally {Pop-Location}
